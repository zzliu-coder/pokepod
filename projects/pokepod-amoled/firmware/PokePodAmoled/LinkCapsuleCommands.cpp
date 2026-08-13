#include "PokePodLinkService.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <algorithm>

#include "BoardServices.h"
#include "CapsuleLibrary.h"
#include "CapsulePolicy.h"
#include "LinkCommandBatchPolicy.h"
#include "TencentWorker.h"

namespace pokepod {
namespace {

constexpr size_t kMaxCommandJsonBytes = 64U * 1024U;
constexpr size_t kCommandReadBytesPerPoll = 4096U;
constexpr uint8_t kBatchPlanNoOp = 1U << 0;
constexpr uint8_t kBatchPlanCapsuleBackup = 1U << 1;
constexpr uint8_t kBatchPlanTrashBackup = 1U << 2;
constexpr uint8_t kBatchPlanProcessingBackup = 1U << 3;

bool batchOperation(const char *operation) {
  return capsuleBatchOperation(operation);
}

cJSON *asJson(void *value) { return static_cast<cJSON *>(value); }

const char *jsonString(cJSON *root, const char *name) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsString(item) && item->valuestring != nullptr
      ? item->valuestring : nullptr;
}

int64_t jsonInt64(cJSON *root, const char *name, int64_t fallback = -1) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsNumber(item) ? static_cast<int64_t>(item->valuedouble)
                              : fallback;
}

String printed(cJSON *root) {
  char *value = cJSON_PrintUnformatted(root);
  const String result = value == nullptr ? String() : String(value);
  cJSON_free(value);
  return result;
}

void replaceStringOrNull(cJSON *root, const char *name, const char *value) {
  cJSON *replacement = value == nullptr ? cJSON_CreateNull()
                                         : cJSON_CreateString(value);
  if (cJSON_HasObjectItem(root, name)) {
    cJSON_ReplaceItemInObjectCaseSensitive(root, name, replacement);
  } else {
    cJSON_AddItemToObject(root, name, replacement);
  }
}

String parentPath(const String &path) {
  const int slash = path.lastIndexOf('/');
  return slash <= 0 ? String("/") : path.substring(0, slash);
}

bool sameUuid(const char *left, const char *right) {
  return left != nullptr && right != nullptr && isUuid(left) && isUuid(right) &&
         strcasecmp(left, right) == 0;
}

}  // namespace

bool PokePodLinkService::startBatchStartupRecovery(
    const String &transactionId) {
  constexpr StorageOwner owner = StorageOwner::capsuleTransaction;
  commandStorageReservation_ = StorageCoordinator::instance().reserve(
      owner, StorageAccess::mutation, 0);
  if (!commandStorageReservation_) return false;
  StoredCapsuleBatchState state;
  const CapsuleBatchJournalStore::LoadResult loaded =
      batchJournalStore_.loadStatus(transactionId, owner, state);
  startupBatchCandidateInvalid_ =
      loaded == CapsuleBatchJournalStore::LoadResult::invalid;
  if (loaded != CapsuleBatchJournalStore::LoadResult::loaded) {
    commandStorageReservation_.release();
    return false;
  }
  prepareCapsuleBatchRecovery(state);
  state.flags &= ~capsuleBatchResponseAllowed;
  if (state.phase == CapsuleBatchPhase::preflight) {
    state.flags &= ~capsuleBatchSuccess;
    state.cursor = state.applied = 0;
    state.phase = CapsuleBatchPhase::result;
  } else if (state.phase == CapsuleBatchPhase::apply) {
    state.flags &= ~capsuleBatchSuccess;
    state.cursor = state.applied;
    state.phase = state.cursor == 0 &&
        (state.flags & capsuleBatchFinalizeApplied) == 0
        ? CapsuleBatchPhase::result : CapsuleBatchPhase::rollback;
  }
  sealCapsuleBatchState(state);
  if (!batchJournalStore_.checkpoint(state, owner)) {
    commandStorageReservation_.release();
    return false;
  }

  LinkCommandExecutor::Phase phase = LinkCommandExecutor::Phase::cleanup;
  if (state.phase == CapsuleBatchPhase::rollback) {
    phase = LinkCommandExecutor::Phase::rollback;
  } else if (state.phase == CapsuleBatchPhase::result) {
    phase = LinkCommandExecutor::Phase::result;
  }
  const bool folderFinalize =
      strcmp(state.operation, "deleteFolderToInbox") == 0;
  if (!batchExecutor_.resume(
          state.total, phase, state.cursor, state.applied,
          (state.flags & capsuleBatchSuccess) != 0,
          (state.flags & capsuleBatchRollbackFailed) != 0,
          folderFinalize,
          (state.flags & capsuleBatchFinalizeApplied) != 0)) {
    commandStorageReservation_.release();
    return false;
  }
  commandStorageActive_ = true;
  batchJournalState_ = state;
  batchTransactionId_ = transactionId;
  batchCommandPath_ = String(kCapsuleSystem) + "/commands/incoming/" +
      transactionId + ".json";
  batchRequestId_ = 0;
  batchJsonRoot_ = nullptr;
  batchMessage_ = (state.flags & capsuleBatchSuccess) != 0
      ? "committed" : "interrupted batch rolled back";
  batchPending_ = BatchPending::none;
  transactionGate_.reset();
  return true;
}

void PokePodLinkService::quarantineBatchJournal(
    const String &transactionId) {
  char suffix[24];
  snprintf(suffix, sizeof(suffix), ".blocked-%08lx",
           static_cast<unsigned long>(esp_random()));
  (void)batchJournalStore_.quarantine(
      transactionId, suffix, storageOwner());
  if (log_ != nullptr) {
    log_->println("{\"event\":\"link_batch_journal_blocked\"}");
  }
}

void PokePodLinkService::advanceBatchStartupRecovery() {
  if (!startupBatchRecoveryPending_ || startupRecoveryFailed_ ||
      batchExecutor_.active()) return;
  if (!startupBatchCandidate_.isEmpty()) {
    if (startBatchStartupRecovery(startupBatchCandidate_)) {
      startupBatchCandidate_ = "";
      startupBatchCandidateFailures_ = 0;
    } else if (startupBatchCandidateInvalid_) {
      quarantineBatchJournal(startupBatchCandidate_);
      startupBatchCandidate_ = "";
      startupBatchCandidateFailures_ = 0;
      startupBatchCandidateInvalid_ = false;
    } else if (++startupBatchCandidateFailures_ >= 3) {
      // An IO/checkpoint failure is still authoritative.  Keep the original
      // journal and fail closed; quarantining it would permit later mutation
      // to be overwritten by a delayed rollback on the next boot.
      startupRecoveryFailed_ = true;
    }
    return;
  }
  if (!startupBatchReservation_) {
    startupBatchReservation_ = StorageCoordinator::instance().reserve(
        storageOwner(), StorageAccess::mutation, 0);
    if (!startupBatchReservation_) return;
  }
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, 0);
  if (!lease) return;
  if (!startupBatchDirectory_) {
    startupBatchDirectory_ = fs_->open(CapsuleBatchJournalStore::kDirectory);
    if (!startupBatchDirectory_ || !startupBatchDirectory_.isDirectory()) {
      if (startupBatchDirectory_) startupBatchDirectory_.close();
      startupBatchReservation_.release();
      startupBatchRecoveryPending_ = false;
      startupPartCleanupPending_ = true;
      return;
    }
  }
  File entry = startupBatchDirectory_.openNextFile();
  if (!entry) {
    startupBatchDirectory_.close();
    startupBatchReservation_.release();
    startupBatchRecoveryPending_ = false;
    startupPartCleanupPending_ = true;
    return;
  }
  const String full = entry.name();
  entry.close();
  const int slash = full.lastIndexOf('/');
  const String name = slash < 0 ? full : full.substring(slash + 1);
  if (!name.endsWith(".cbj")) return;
  const String transactionId = name.substring(0, name.length() - 4);
  startupBatchDirectory_.close();
  startupBatchReservation_.release();
  lease.release();
  if (capsuleBatchUuid(transactionId.c_str())) {
    startupBatchCandidate_ = transactionId;
    startupBatchCandidateFailures_ = 0;
  } else {
    const String source = String(CapsuleBatchJournalStore::kDirectory) +
        "/" + name;
    char suffix[24];
    snprintf(suffix, sizeof(suffix), ".blocked-%08lx",
             static_cast<unsigned long>(esp_random()));
    (void)storageRename(source, source + suffix);
  }
}

void PokePodLinkService::handleCommandFile(uint32_t requestId,
                                           const String &path,
                                           const String &transactionId) {
  commandStorageReservation_ = StorageCoordinator::instance().reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, storageIoTimeout());
  if (!commandStorageReservation_) {
    sendError(requestId, "storage busy");
    return;
  }
  commandStorageActive_ = true;
  const String resultPath = String(kCapsuleSystem) + "/commands/results/" +
      transactionId + ".json";
  const bool alreadyComplete = storageExists(resultPath, StorageAccess::read);
  if (mutationRecoveryBlocked_ && !alreadyComplete) {
    commandStorageActive_ = false;
    commandStorageReservation_.release();
    sendBusy(requestId, 1000);
    return;
  }
  if (beginCommandLoad(requestId, path, transactionId, alreadyComplete)) return;
  commandStorageActive_ = false;
  commandStorageReservation_.release();
  operation_.releaseResource(LinkOperationResource::transaction);
  operation_.releaseResource(LinkOperationResource::storageReservation);
  sendError(requestId, "command load failed");
}

bool PokePodLinkService::beginCommandLoad(
    uint32_t requestId, const String &path, const String &transactionId,
    bool alreadyComplete) {
  if (fs_ == nullptr || commandLoadState_ != CommandLoadState::none) {
    return false;
  }
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::read, storageIoTimeout());
  if (!lease) return false;
  File file = fs_->open(path, FILE_READ);
  const size_t bytes = file ? file.size() : 0;
  if (!file || file.isDirectory() || bytes == 0 ||
      bytes > kMaxCommandJsonBytes ||
      !commandLoadValue_.reserve(bytes + 1)) {
    if (file) file.close();
    return false;
  }
  commandLoadFile_ = file;
  commandLoadPath_ = path;
  commandLoadTransactionId_ = transactionId;
  commandLoadRequestId_ = requestId;
  commandLoadExpected_ = static_cast<uint32_t>(bytes);
  commandLoadRespond_ = sessionActive_ && transferPermitted();
  commandLoadAlreadyComplete_ = alreadyComplete;
  commandLoadState_ = CommandLoadState::reading;
  if (operationOwns(requestId)) {
    operation_.advance(LinkOperationState::processing);
    operation_.ownResource(LinkOperationResource::transaction);
    operation_.ownResource(LinkOperationResource::storageReservation);
    operation_.ownResource(LinkOperationResource::file);
  }
  return true;
}

void PokePodLinkService::advanceCommandLoad() {
  if (commandLoadState_ == CommandLoadState::none) return;
  if (commandLoadState_ == CommandLoadState::dispatch) {
    dispatchLoadedCommand();
    return;
  }
  const bool permitted = commandLoadRespond_ && sessionActive_ &&
      transferPermitted();
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::read, 0);
  if (!lease) return;
  if (!permitted || !commandLoadFile_) {
    if (commandLoadFile_) commandLoadFile_.close();
    lease.release();
    finishCommandLoad(true);
    return;
  }
  if (payload_ == nullptr) {
    commandLoadFile_.close();
    lease.release();
    finishCommandLoad(true);
    return;
  }
  const int available = commandLoadFile_.available();
  if (available <= 0) {
    commandLoadFile_.close();
    if (commandLoadValue_.length() != commandLoadExpected_ ||
        !transferPermitted()) {
      lease.release();
      finishCommandLoad(true);
      return;
    }
    commandLoadState_ = CommandLoadState::dispatch;
    return;
  }
  const size_t wanted = std::min<size_t>(
      kCommandReadBytesPerPoll, static_cast<size_t>(available));
  const int received = commandLoadFile_.read(payload_, wanted);
  if (received <= 0 || !commandLoadValue_.concat(
          reinterpret_cast<const char *>(payload_),
          static_cast<unsigned int>(received))) {
    commandLoadFile_.close();
    lease.release();
    finishCommandLoad(true);
    return;
  }
  if (!transferPermitted()) commandLoadRespond_ = false;
}

void PokePodLinkService::finishCommandLoad(bool keepCommand) {
  if (!keepCommand && !commandLoadPath_.isEmpty()) {
    queueDeferredTreeCleanup(commandLoadPath_);
  }
  commandLoadFile_ = File();
  commandLoadValue_ = "";
  commandLoadPath_ = "";
  commandLoadTransactionId_ = "";
  commandLoadRequestId_ = 0;
  commandLoadExpected_ = 0;
  commandLoadRespond_ = false;
  commandLoadAlreadyComplete_ = false;
  commandLoadState_ = CommandLoadState::none;
  operation_.releaseResource(LinkOperationResource::file);
  commandStorageActive_ = false;
  commandStorageReservation_.release();
  operation_.releaseResource(LinkOperationResource::transaction);
  operation_.releaseResource(LinkOperationResource::storageReservation);
}

void PokePodLinkService::dispatchLoadedCommand() {
  if (!commandLoadRespond_ || !sessionActive_ || !transferPermitted()) {
    finishCommandLoad(true);
    return;
  }
  cJSON *root = cJSON_ParseWithLength(
      commandLoadValue_.c_str(), commandLoadValue_.length());
  if (!transferPermitted()) {
    cJSON_Delete(root);
    finishCommandLoad(true);
    return;
  }
  const uint32_t requestId = commandLoadRequestId_;
  const String path = commandLoadPath_;
  const String transactionId = commandLoadTransactionId_;
  if (commandLoadAlreadyComplete_) {
    applyCompletedCommandSideEffects(root);
    cJSON_Delete(root);
    const uint32_t cleanupRequest = commandLoadRespond_ && sessionActive_ &&
        transferPermitted() ? requestId : 0;
    queueDeferredTreeCleanup(path);
    commandCleanupPending_ = true;
    commandCleanupRequestId_ = cleanupRequest;
    commandLoadFile_ = File();
    commandLoadValue_ = "";
    commandLoadPath_ = "";
    commandLoadTransactionId_ = "";
    commandLoadRequestId_ = 0;
    commandLoadExpected_ = 0;
    commandLoadRespond_ = false;
    commandLoadAlreadyComplete_ = false;
    commandLoadState_ = CommandLoadState::none;
    operation_.releaseResource(LinkOperationResource::file);
    return;
  }
  const BatchStart batch = tryStartBatchCommand(
      requestId, root, path, transactionId);
  if (batch == BatchStart::started) {
    commandLoadFile_ = File();
    commandLoadValue_ = "";
    commandLoadPath_ = "";
    commandLoadTransactionId_ = "";
    commandLoadRequestId_ = 0;
    commandLoadExpected_ = 0;
    commandLoadRespond_ = false;
    commandLoadAlreadyComplete_ = false;
    commandLoadState_ = CommandLoadState::none;
    operation_.releaseResource(LinkOperationResource::file);
    return;
  }
  if (batch == BatchStart::rejected) {
    cJSON_Delete(root);
    finishCommandLoad(true);
    return;
  }
  if (tryStartTextCommand(requestId, root, path, transactionId)) {
    cJSON_Delete(root);
    commandLoadFile_ = File();
    commandLoadValue_ = "";
    commandLoadPath_ = "";
    commandLoadTransactionId_ = "";
    commandLoadRequestId_ = 0;
    commandLoadExpected_ = 0;
    commandLoadRespond_ = false;
    commandLoadAlreadyComplete_ = false;
    commandLoadState_ = CommandLoadState::none;
    operation_.releaseResource(LinkOperationResource::file);
    return;
  }
  const bool validEnvelope = root != nullptr && cJSON_IsObject(root) &&
      jsonInt64(root, "schemaVersion") == 2 &&
      sameUuid(jsonString(root, "transactionId"), transactionId.c_str());
  const char *message = validEnvelope
      ? "command operation is not supported by this firmware"
      : "command JSON is malformed";
  const bool accepted = startCommandFailureResult(
      requestId, path, transactionId, message);
  cJSON_Delete(root);
  if (!accepted && log_ != nullptr) {
    log_->printf(
        "{\"event\":\"link_command_failed\",\"transaction\":\"%s\","
        "\"message\":\"%s\"}\n",
        transactionId.c_str(), message);
  }
  commandLoadFile_ = File();
  commandLoadValue_ = "";
  commandLoadPath_ = "";
  commandLoadTransactionId_ = "";
  commandLoadRequestId_ = 0;
  commandLoadExpected_ = 0;
  commandLoadRespond_ = false;
  commandLoadAlreadyComplete_ = false;
  commandLoadState_ = CommandLoadState::none;
  operation_.releaseResource(LinkOperationResource::file);
  if (!accepted) {
    commandStorageActive_ = false;
    commandStorageReservation_.release();
    operation_.releaseResource(LinkOperationResource::transaction);
    operation_.releaseResource(LinkOperationResource::storageReservation);
  }
}

bool PokePodLinkService::tryStartTextCommand(
    uint32_t requestId, void *jsonRoot, const String &path,
    const String &transactionId) {
  cJSON *root = asJson(jsonRoot);
  const char *operation = root == nullptr ? nullptr :
      jsonString(root, "operation");
  const bool correction = operation != nullptr &&
      strcmp(operation, "commitCorrection") == 0;
  const bool finalText = operation != nullptr &&
      strcmp(operation, "commitFinalText") == 0;
  if (!correction && !finalText) {
    return false;
  }
  cJSON *ids = cJSON_GetObjectItemCaseSensitive(root, "capsuleIds");
  const char *id = cJSON_IsArray(ids) && cJSON_GetArraySize(ids) == 1
      ? cJSON_GetStringValue(cJSON_GetArrayItem(ids, 0)) : nullptr;
  const char *maintenanceId = jsonString(root, "maintenanceId");
  const char *stagedPath = jsonString(root, "stagedPath");
  const CapsuleSummary *record = isUuid(id) ? library_->find(id) : nullptr;
  const String expectedStaged = ".staging/" + transactionId + "/" +
      (id == nullptr ? "" : id);
  const String stagedDirectory = String(kCapsuleRoot) + "/" + expectedStaged;
  const String prepared = stagedDirectory +
      (correction ? "/polished.md" : "/final.md");
  if (jsonInt64(root, "schemaVersion") != 2 ||
      !sameUuid(jsonString(root, "transactionId"), transactionId.c_str()) ||
      record == nullptr || activeMaintenance_.isEmpty() ||
      !isUuid(maintenanceId) ||
      !activeMaintenance_.equalsIgnoreCase(maintenanceId)) {
    return false;
  }

  const int expectedRevision = static_cast<int>(
      jsonInt64(root, "expectedRevision"));
  String metadataTarget;
  cJSON *metadata = nullptr;
  if (correction) {
    if (stagedPath == nullptr || String(stagedPath) != expectedStaged ||
        !storageExists(prepared, StorageAccess::read)) {
      return false;
    }
    metadataTarget = record->directory + "/processing.json";
    metadata = cJSON_Parse(readText(metadataTarget, 8192).c_str());
  } else {
    metadataTarget = record->directory + "/capsule.json";
    metadata = cJSON_Parse(readText(metadataTarget, 8192).c_str());
  }
  cJSON *revision = metadata == nullptr ? nullptr :
      cJSON_GetObjectItemCaseSensitive(metadata, "revision");
  if (!cJSON_IsNumber(revision) || revision->valueint != expectedRevision) {
    cJSON_Delete(metadata);
    return false;
  }
  cJSON_SetNumberValue(revision, expectedRevision + 1);
  if (correction) {
    cJSON_ReplaceItemInObjectCaseSensitive(
        metadata, "status", cJSON_CreateString("ready"));
    replaceStringOrNull(metadata, "polishedTextFile", "polished.md");
    replaceStringOrNull(metadata, "errorStage", nullptr);
    replaceStringOrNull(metadata, "error", nullptr);
  } else {
    replaceStringOrNull(metadata, "updatedAt", board_->utcNow().c_str());
  }
  commandTextMetadataValue_ = printed(metadata) + "\n";
  cJSON_Delete(metadata);

  CapsuleTransactionInput inputs[2];
  const String textTarget = record->directory +
      (correction ? "/polished.md" : "/final.md");
  if (stagedPath != nullptr && String(stagedPath) == expectedStaged &&
      storageExists(prepared, StorageAccess::read)) {
    inputs[0] = {textTarget, nullptr, prepared};
  } else if (finalText && jsonString(root, "finalText") != nullptr &&
             strlen(jsonString(root, "finalText")) <= 8192U) {
    commandTextValue_ = jsonString(root, "finalText");
    commandTextSource_.bind(commandTextValue_);
    inputs[0] = {textTarget, &commandTextSource_, String()};
  } else {
    return false;
  }
  commandTextMetadataSource_.bind(commandTextMetadataValue_);
  inputs[1] = {metadataTarget, &commandTextMetadataSource_, String()};
  const bool hadStagedText = stagedPath != nullptr &&
      String(stagedPath) == expectedStaged;
  const bool started = transactionRunner_.startCommit(
      transactionId.c_str(), inputs, 2, storageOwner());
  if (!started) return false;
  transactionGate_.beginOperation(transferGate_);
  transactionPurpose_ = TransactionPurpose::commandText;
  commandTextRequestId_ = requestId;
  commandTextPath_ = path;
  commandTextTransactionId_ = transactionId;
  commandTextStagingDirectory_ = hadStagedText ? stagedDirectory : String();
  commandTextRespond_ = sessionActive_ && transferPermitted();
  return true;
}

void PokePodLinkService::finishTextCommand(bool committed) {
  cJSON *result = cJSON_CreateObject();
  cJSON_AddNumberToObject(result, "schemaVersion", 1);
  cJSON_AddStringToObject(result, "commandId",
                          commandTextTransactionId_.c_str());
  cJSON_AddStringToObject(result, "transactionId",
                          commandTextTransactionId_.c_str());
  cJSON_AddBoolToObject(result, "ok", committed);
  cJSON_AddBoolToObject(result, "success", committed);
  cJSON_AddStringToObject(result, "message",
                          committed ? "committed" : "text commit failed");
  cJSON_AddStringToObject(result, "completedAt", board_->utcNow().c_str());
  const String resultPath = String(kCapsuleSystem) + "/commands/results/" +
      commandTextTransactionId_ + ".json";
  commandTextMetadataValue_ = printed(result) + "\n";
  cJSON_Delete(result);
  transactionPurpose_ = TransactionPurpose::none;
  transactionGate_.reset();
  commandTextMetadataSource_.bind(commandTextMetadataValue_);
  const CapsuleTransactionInput input{
      resultPath, &commandTextMetadataSource_, String()};
  const String key = commandTextTransactionId_ + "-result";
  if (!transactionRunner_.startCommit(key.c_str(), &input, 1,
                                      storageOwner())) {
    finishTextResult(false);
    return;
  }
  // Result durability is local recovery work.  A Wi-Fi deadline or USB
  // disconnect suppresses the response, but it must not leave a published
  // text mutation without its idempotency result.
  transactionPurpose_ = TransactionPurpose::commandTextResult;
}

void PokePodLinkService::finishTextResult(bool persisted) {
  const uint32_t requestId = commandTextRequestId_;
  const bool respond = persisted && commandTextRespond_ && sessionActive_ &&
      transferPermitted();
  transactionPurpose_ = TransactionPurpose::none;
  if (persisted) {
    if (!commandTextStagingDirectory_.isEmpty()) {
      queueDeferredTreeCleanup(commandTextStagingDirectory_);
    }
    queueDeferredTreeCleanup(commandTextPath_);
    commandCleanupPending_ = true;
    commandCleanupRequestId_ = respond ? requestId : 0;
  } else {
    // Keep the command and staged input as replay evidence.  No product
    // mutation is acknowledged without the durable result file.
    commandStorageActive_ = false;
    commandStorageReservation_.release();
    operation_.releaseResource(LinkOperationResource::storageReservation);
  }
  operation_.releaseResource(LinkOperationResource::transaction);
  commandTextRequestId_ = 0;
  commandTextPath_ = "";
  commandTextTransactionId_ = "";
  commandTextStagingDirectory_ = "";
  commandTextValue_ = "";
  commandTextMetadataValue_ = "";
  commandTextRespond_ = false;
  (void)library_->requestScan();
}

bool PokePodLinkService::startCommandFailureResult(
    uint32_t requestId, const String &path, const String &transactionId,
    const char *message) {
  cJSON *result = cJSON_CreateObject();
  cJSON_AddNumberToObject(result, "schemaVersion", 1);
  cJSON_AddStringToObject(result, "commandId", transactionId.c_str());
  cJSON_AddStringToObject(result, "transactionId", transactionId.c_str());
  cJSON_AddBoolToObject(result, "ok", false);
  cJSON_AddBoolToObject(result, "success", false);
  cJSON_AddStringToObject(result, "message",
                          message == nullptr ? "command failed" : message);
  cJSON_AddStringToObject(result, "completedAt", board_->utcNow().c_str());
  commandTextMetadataValue_ = printed(result) + "\n";
  cJSON_Delete(result);
  const String resultPath = String(kCapsuleSystem) + "/commands/results/" +
      transactionId + ".json";
  commandTextMetadataSource_.bind(commandTextMetadataValue_);
  const CapsuleTransactionInput input{
      resultPath, &commandTextMetadataSource_, String()};
  const String key = transactionId + "-result";
  if (!transactionRunner_.startCommit(key.c_str(), &input, 1,
                                      storageOwner())) return false;
  transactionPurpose_ = TransactionPurpose::commandFailureResult;
  commandTextRequestId_ = requestId;
  commandTextPath_ = path;
  commandTextTransactionId_ = transactionId;
  commandTextRespond_ = sessionActive_ && transferPermitted();
  return true;
}

void PokePodLinkService::finishCommandFailureResult(bool persisted) {
  const uint32_t requestId = commandTextRequestId_;
  const bool respond = persisted && commandTextRespond_ && sessionActive_ &&
      transferPermitted();
  transactionPurpose_ = TransactionPurpose::none;
  if (persisted) {
    queueDeferredTreeCleanup(commandTextPath_);
    commandCleanupPending_ = true;
    commandCleanupRequestId_ = respond ? requestId : 0;
  } else {
    commandStorageActive_ = false;
    commandStorageReservation_.release();
    operation_.releaseResource(LinkOperationResource::storageReservation);
  }
  operation_.releaseResource(LinkOperationResource::transaction);
  commandTextRequestId_ = 0;
  commandTextPath_ = "";
  commandTextTransactionId_ = "";
  commandTextMetadataValue_ = "";
  commandTextRespond_ = false;
}

PokePodLinkService::BatchStart PokePodLinkService::tryStartBatchCommand(
    uint32_t requestId, void *jsonRoot, const String &path,
    const String &transactionId) {
  cJSON *root = asJson(jsonRoot);
  const char *operation = root == nullptr ? nullptr :
      jsonString(root, "operation");
  if (root == nullptr || !cJSON_IsObject(root) ||
      operation == nullptr || !batchOperation(operation)) {
    return BatchStart::notApplicable;
  }
  cJSON *ids = cJSON_GetObjectItemCaseSensitive(root, "capsuleIds");
  const bool simple = capsuleBatchSimpleOperation(operation);
  const int count = simple ? 1 :
      (cJSON_IsArray(ids) ? cJSON_GetArraySize(ids) : -1);
  const char *maintenanceId = jsonString(root, "maintenanceId");
  const bool deleteFolder = strcmp(operation, "deleteFolderToInbox") == 0;
  const char *finalizeFolder = deleteFolder
      ? jsonString(root, "folderPath") : nullptr;
  const char *rejection = nullptr;
  if (jsonInt64(root, "schemaVersion") != 2 ||
      !sameUuid(jsonString(root, "transactionId"), transactionId.c_str())) {
    rejection = "invalid batch command";
  } else if (count < 0 ||
             count > static_cast<int>(LinkCommandExecutor::kMaximumItems)) {
    rejection = "invalid batch size";
  } else if (strcmp(operation, "beginMaintenance") == 0 &&
             (!isUuid(maintenanceId) ||
              (!activeMaintenance_.isEmpty() &&
               !activeMaintenance_.equalsIgnoreCase(maintenanceId)))) {
    rejection = "another maintenance session is active";
  } else if (strcmp(operation, "beginMaintenance") != 0 &&
             strcmp(operation, "rescan") != 0 &&
             (activeMaintenance_.isEmpty() || !isUuid(maintenanceId) ||
              !activeMaintenance_.equalsIgnoreCase(maintenanceId))) {
    rejection = "maintenance session does not own the device";
  } else if (deleteFolder && !safeFolder(finalizeFolder, false)) {
    rejection = "invalid folder delete";
  } else if (!simple && batchSeenIds_ == nullptr &&
             (batchSeenIds_ = static_cast<uint8_t *>(heap_caps_calloc(
                  1024, 17, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) == nullptr) {
    rejection = "batch identity table unavailable";
  } else if (!batchJournalStore_.create(
                 transactionId, operation, static_cast<uint16_t>(count),
                 storageOwner(), batchJournalState_, finalizeFolder)) {
    rejection = "batch journal unavailable";
  }
  // A recognized command never falls through to the synchronous legacy path.
  // Validation/journal failures are represented as an empty failed executor;
  // it persists the normal durable command result before acknowledging upload.
  const bool executorStarted = rejection == nullptr
      ? batchExecutor_.begin(static_cast<size_t>(count), deleteFolder)
      : batchExecutor_.beginRejected();
  if (!executorStarted) {
    return BatchStart::rejected;
  }
  batchJsonRoot_ = root;
  batchRequestId_ = requestId;
  batchCommandPath_ = path;
  batchTransactionId_ = transactionId;
  batchMessage_ = rejection == nullptr ? "committed" : rejection;
  batchPending_ = BatchPending::none;
  batchPendingStep_ = 0;
  batchNextIdItem_ = cJSON_IsArray(ids) ? ids->child : nullptr;
  batchNextIdIndex_ = 0;
  if (!simple && batchSeenIds_ != nullptr) memset(batchSeenIds_, 0, 1024 * 17);
  transactionGate_.beginOperation(transferGate_);
  log_->printf(
      "{\"event\":\"link_batch_begin\",\"transaction\":\"%s\","
      "\"operation\":\"%s\",\"count\":%u}\n",
      transactionId.c_str(), operation, static_cast<unsigned>(count));
  return BatchStart::started;
}

void PokePodLinkService::updateBatchJournalState(bool itemInFlight) {
  switch (batchExecutor_.phase()) {
    case LinkCommandExecutor::Phase::preflight:
      batchJournalState_.phase = CapsuleBatchPhase::preflight;
      break;
    case LinkCommandExecutor::Phase::apply:
    case LinkCommandExecutor::Phase::finalize:
      batchJournalState_.phase = CapsuleBatchPhase::apply;
      break;
    case LinkCommandExecutor::Phase::rollback:
      batchJournalState_.phase = CapsuleBatchPhase::rollback;
      break;
    case LinkCommandExecutor::Phase::result:
      batchJournalState_.phase = CapsuleBatchPhase::result;
      break;
    case LinkCommandExecutor::Phase::cleanup:
    case LinkCommandExecutor::Phase::finished:
    case LinkCommandExecutor::Phase::idle:
      batchJournalState_.phase = CapsuleBatchPhase::cleanup;
      break;
  }
  batchJournalState_.cursor = static_cast<uint16_t>(batchExecutor_.cursor());
  batchJournalState_.applied = static_cast<uint16_t>(batchExecutor_.applied());
  batchJournalState_.flags = 0;
  if (batchExecutor_.success()) {
    batchJournalState_.flags |= capsuleBatchSuccess;
  }
  if (batchExecutor_.responseAllowed()) {
    batchJournalState_.flags |= capsuleBatchResponseAllowed;
  }
  if (batchExecutor_.rollbackFailed()) {
    batchJournalState_.flags |= capsuleBatchRollbackFailed;
  }
  if (batchExecutor_.finalizeApplied()) {
    batchJournalState_.flags |= capsuleBatchFinalizeApplied;
  }
  if (itemInFlight) batchJournalState_.flags |= capsuleBatchItemInFlight;
  sealCapsuleBatchState(batchJournalState_);
}

String PokePodLinkService::batchArtifactPath(size_t index,
                                              const char *suffix) const {
  return String(CapsuleBatchJournalStore::kDirectory) + "/" +
      batchTransactionId_ + "-" + String(static_cast<unsigned>(index)) +
      (suffix == nullptr ? "" : suffix);
}

String PokePodLinkService::batchPurgePath(
    const StoredCapsuleBatchPlan &plan) const {
  return String(kCapsuleStaging) + "/purge-batch-" +
      batchTransactionId_ + "-" + plan.id;
}

String PokePodLinkService::batchFolderStagingPath() const {
  return String(kCapsuleStaging) + "/folder-batch-" + batchTransactionId_;
}

void PokePodLinkService::advanceBatchCommand() {
  if (!batchExecutor_.active()) return;
  if (batchPending_ != BatchPending::none) {
    advanceBatchPending();
    return;
  }
  const LinkCommandExecutor::Work work =
      batchExecutor_.poll(batchForegroundPermitted());
  if (work.action == LinkCommandExecutor::Action::none) return;
  batchWork_ = work;
  if (!startBatchWork(work)) finishBatchWork(false);
}

bool PokePodLinkService::startBatchWork(
    const LinkCommandExecutor::Work &work) {
  switch (work.action) {
    case LinkCommandExecutor::Action::checkpoint:
      updateBatchJournalState(batchExecutor_.checkpointMarksItemInFlight());
      finishBatchWork(batchJournalStore_.checkpoint(
          batchJournalState_, storageOwner()));
      return true;
    case LinkCommandExecutor::Action::preflightItem:
      return startBatchPreflight(work.index);
    case LinkCommandExecutor::Action::applyItem:
      return startBatchApply(work.index);
    case LinkCommandExecutor::Action::finalizeApply:
      return startBatchFinalize();
    case LinkCommandExecutor::Action::rollbackFinalize:
      return startBatchRollbackFinalize();
    case LinkCommandExecutor::Action::rollbackItem:
      return startBatchRollback(work.index);
    case LinkCommandExecutor::Action::persistResult:
      return startBatchResultPersistence();
    case LinkCommandExecutor::Action::cleanup:
      if (!cleanupBatchArtifacts()) {
        finishBatchWork(false);
      } else if (batchPending_ == BatchPending::none) {
        finishBatchWork(true);
      }
      return true;
    case LinkCommandExecutor::Action::finish:
      finishBatchCommand();
      finishBatchWork(true);
      return true;
    case LinkCommandExecutor::Action::none:
      return true;
  }
  return false;
}

void PokePodLinkService::finishBatchWork(bool ok) {
  batchPending_ = BatchPending::none;
  batchPendingStep_ = 0;
  batchExecutor_.completeStep(ok);
}

bool PokePodLinkService::rememberBatchId(const char *uuid) {
  if (batchSeenIds_ == nullptr || !isUuid(uuid)) return false;
  uint8_t value[16] = {};
  size_t byte = 0;
  uint8_t high = 0;
  bool haveHigh = false;
  for (const char *cursor = uuid; *cursor != '\0'; ++cursor) {
    if (*cursor == '-') continue;
    const char c = *cursor;
    const uint8_t nibble = c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10;
    if (!haveHigh) {
      high = static_cast<uint8_t>(nibble << 4);
      haveHigh = true;
    } else {
      if (byte >= sizeof(value)) return false;
      value[byte++] = static_cast<uint8_t>(high | nibble);
      haveHigh = false;
    }
  }
  if (byte != sizeof(value) || haveHigh) return false;
  uint32_t hash = 2166136261U;
  for (const uint8_t octet : value) hash = (hash ^ octet) * 16777619U;
  for (size_t probe = 0; probe < 1024; ++probe) {
    uint8_t *slot = batchSeenIds_ + (((hash + probe) & 1023U) * 17U);
    if (slot[0] == 0) {
      slot[0] = 1;
      memcpy(slot + 1, value, sizeof(value));
      return true;
    }
    if (memcmp(slot + 1, value, sizeof(value)) == 0) return false;
  }
  return false;
}

bool PokePodLinkService::buildBatchPlan(
    size_t index, StoredCapsuleBatchPlan &plan, String &message) {
  cJSON *root = asJson(batchJsonRoot_);
  const char *operation = root == nullptr ? nullptr :
      jsonString(root, "operation");
  if (capsuleBatchSimpleOperation(operation)) {
    return index == 0 && buildSimpleBatchPlan(root, plan, message);
  }
  cJSON *item = index == batchNextIdIndex_ ?
      asJson(batchNextIdItem_) : nullptr;
  const char *rawId = cJSON_GetStringValue(item);
  if (!isUuid(rawId)) {
    message = "invalid capsule id";
    return false;
  }
  if (!rememberBatchId(rawId)) {
    message = "duplicate capsule id";
    return false;
  }
  batchNextIdItem_ = item->next;
  ++batchNextIdIndex_;
  String id(rawId);
  id.toLowerCase();

  operation = batchJournalState_.operation;
  const bool restore = strcmp(operation, "restoreCapsules") == 0;
  const bool purge = strcmp(operation, "purgeCapsules") == 0;
  const bool remove = strcmp(operation, "deleteCapsules") == 0;
  const bool copy = strcmp(operation, "copyCapsules") == 0;
  const bool move = strcmp(operation, "moveCapsules") == 0;
  const bool evacuate = strcmp(operation, "deleteFolderToInbox") == 0;
  const bool metadata = strcmp(operation, "setFavorite") == 0 ||
      strcmp(operation, "addTags") == 0 ||
      strcmp(operation, "removeTags") == 0 ||
      strcmp(operation, "renameTag") == 0 ||
      strcmp(operation, "mergeTag") == 0 ||
      strcmp(operation, "deleteTag") == 0;

  String source;
  String target;
  String targetId = id;
  if (restore || purge) {
    source = String(kCapsuleTrash) + "/" + id;
  } else if (evacuate) {
    const char *folder = jsonString(root, "folderPath");
    if (!safeFolder(folder, false)) {
      message = "invalid user folder";
      return false;
    }
    const String folderRoot = String(kCapsuleRoot) + "/" + folder + "/";
    source = activeCapsuleDirectory(id);
    if (!source.startsWith(folderRoot)) {
      message = "folder contents changed during preflight";
      return false;
    }
  } else {
    source = activeCapsuleDirectory(id);
  }
  if (source.isEmpty() || !storageExists(source, StorageAccess::read)) {
    message = "capsule is missing";
    return false;
  }

  if (move || copy) {
    const String folder = folderDirectory(jsonString(root, "destination"));
    if (folder.isEmpty() || !storageExists(folder, StorageAccess::read)) {
      message = "invalid move destination";
      return false;
    }
    if (copy) targetId = newUuid();
    target = folder + "/" + targetId;
  } else if (remove) {
    target = String(kCapsuleTrash) + "/" + id;
  } else if (restore) {
    cJSON *trash = cJSON_Parse(readText(source + "/trash.json", 8192).c_str());
    const char *folderName = trash == nullptr ? nullptr :
        jsonString(trash, "originalFolder");
    String folder = folderDirectory(folderName);
    cJSON_Delete(trash);
    if (folder.isEmpty() || !storageExists(folder, StorageAccess::read)) {
      folder = kCapsuleInbox;
    }
    target = folder + "/" + id;
  } else if (evacuate) {
    target = String(kCapsuleInbox) + "/" + id;
  }
  const bool noOp = move && source == target;
  if (!target.isEmpty() && !noOp &&
      storageExists(target, StorageAccess::read)) {
    message = "capsule target already exists";
    return false;
  }

  cJSON *expected = cJSON_GetObjectItemCaseSensitive(
      root, "expectedRevisions");
  cJSON *wanted = cJSON_IsObject(expected) ?
      cJSON_GetObjectItemCaseSensitive(expected, id.c_str()) : nullptr;
  const String revisionPath = source +
      (restore || purge ? "/trash.json" : "/capsule.json");
  cJSON *revisionRoot = cJSON_Parse(readText(revisionPath, 8192).c_str());
  cJSON *revision = revisionRoot == nullptr ? nullptr :
      cJSON_GetObjectItemCaseSensitive(revisionRoot, "revision");
  const bool revisionMatches = cJSON_IsNumber(wanted) &&
      cJSON_IsNumber(revision) && wanted->valueint == revision->valueint;
  const int revisionValue = cJSON_IsNumber(revision) ? revision->valueint : -1;
  cJSON_Delete(revisionRoot);
  if (!revisionMatches) {
    message = "revision conflict for " + id;
    return false;
  }

  memset(&plan, 0, sizeof(plan));
  strlcpy(plan.id, id.c_str(), sizeof(plan.id));
  strlcpy(plan.targetId, targetId.c_str(), sizeof(plan.targetId));
  strlcpy(plan.source, source.c_str(), sizeof(plan.source));
  if (!target.isEmpty()) strlcpy(plan.target, target.c_str(), sizeof(plan.target));
  plan.expectedRevision = revisionValue;
  if (noOp) plan.flags |= kBatchPlanNoOp;
  if ((move || restore || evacuate || metadata) && !noOp) {
    plan.flags |= kBatchPlanCapsuleBackup;
  }
  if (restore) plan.flags |= kBatchPlanTrashBackup;
  sealCapsuleBatchPlan(plan);
  if (!validCapsuleBatchPlan(plan, operation)) {
    message = "batch plan is invalid";
    return false;
  }
  return true;
}

bool PokePodLinkService::buildSimpleBatchPlan(
    void *jsonRoot, StoredCapsuleBatchPlan &plan, String &message) {
  cJSON *root = asJson(jsonRoot);
  const char *operation = root == nullptr ? nullptr :
      jsonString(root, "operation");
  if (!capsuleBatchSimpleOperation(operation)) return false;
  memset(&plan, 0, sizeof(plan));
  strlcpy(plan.id, batchTransactionId_.c_str(), sizeof(plan.id));

  const char *maintenanceId = jsonString(root, "maintenanceId");
  if (strcmp(operation, "rescan") == 0) {
    strlcpy(plan.targetId, batchTransactionId_.c_str(),
            sizeof(plan.targetId));
  } else if (strcmp(operation, "beginMaintenance") == 0 ||
      strcmp(operation, "endMaintenance") == 0) {
    const bool begin = operation[0] == 'b';
    if (!isUuid(maintenanceId) ||
        (begin && !activeMaintenance_.isEmpty() &&
         !activeMaintenance_.equalsIgnoreCase(maintenanceId)) ||
        (!begin && (activeMaintenance_.isEmpty() ||
         !activeMaintenance_.equalsIgnoreCase(maintenanceId)))) {
      message = begin ? "another maintenance session is active" :
                        "maintenance session does not own the device";
      return false;
    }
    if (!activeMaintenance_.isEmpty()) {
      strlcpy(plan.source, activeMaintenance_.c_str(), sizeof(plan.source));
    }
    strlcpy(plan.targetId, maintenanceId, sizeof(plan.targetId));
  } else if (strcmp(operation, "createFolder") == 0 ||
             strcmp(operation, "renameFolder") == 0) {
    const char *folder = jsonString(root, "folderPath");
    if (!safeFolder(folder, false)) {
      message = "invalid user folder";
      return false;
    }
    const String source = String(kCapsuleRoot) + "/" + folder;
    const bool rename = operation[0] == 'r';
    if ((!rename && storageExists(source, StorageAccess::read)) ||
        (rename && !storageExists(source, StorageAccess::read))) {
      message = rename ? "invalid folder rename" : "folder already exists";
      return false;
    }
    strlcpy(plan.source, source.c_str(), sizeof(plan.source));
    if (rename) {
      const char *newFolder = jsonString(root, "newFolderPath");
      if (!safeFolder(newFolder, false)) {
        message = "invalid folder rename";
        return false;
      }
      const String target = String(kCapsuleRoot) + "/" + newFolder;
      if (storageExists(target, StorageAccess::read) ||
          !storageExists(parentPath(target), StorageAccess::read)) {
        message = "folder rename target is invalid";
        return false;
      }
      strlcpy(plan.target, target.c_str(), sizeof(plan.target));
    } else if (!storageExists(parentPath(source), StorageAccess::read)) {
      message = "folder parent is missing";
      return false;
    }
  } else {
    cJSON *ids = cJSON_GetObjectItemCaseSensitive(root, "capsuleIds");
    const char *id = cJSON_IsArray(ids) && cJSON_GetArraySize(ids) == 1
        ? cJSON_GetStringValue(cJSON_GetArrayItem(ids, 0)) : nullptr;
    if (!isUuid(id)) {
      message = "single capsule id is required";
      return false;
    }
    strlcpy(plan.id, id, sizeof(plan.id));
    strlcpy(plan.targetId, id, sizeof(plan.targetId));
    if (strcmp(operation, "requeueTranscription") == 0) {
      const CapsuleSummary *record = library_->find(id);
      cJSON *expected = cJSON_GetObjectItemCaseSensitive(
          root, "expectedRevisions");
      cJSON *wanted = cJSON_IsObject(expected) ?
          cJSON_GetObjectItemCaseSensitive(expected, id) : nullptr;
      if (record == nullptr || record->readOnly || !cJSON_IsNumber(wanted) ||
          wanted->valueint != record->processingRevision ||
          record->status != CapsuleStatus::failed) {
        message = "processing revision conflict";
        return false;
      }
      strlcpy(plan.source, record->directory.c_str(), sizeof(plan.source));
      plan.expectedRevision = record->processingRevision;
      plan.flags |= kBatchPlanProcessingBackup;
    } else {
      const char *stagedPath = jsonString(root, "stagedPath");
      const char *destination = jsonString(root, "destination");
      const String expected = ".staging/" + batchTransactionId_ + "/" + id;
      const String source = String(kCapsuleRoot) + "/" +
          (stagedPath == nullptr ? "" : stagedPath);
      const String targetFolder = folderDirectory(destination);
      const String target = targetFolder + "/" + id;
      const String capsuleText = readText(source + "/capsule.json", 8192);
      const String processingText = readText(
          source + "/processing.json", 8192);
      cJSON *capsule = cJSON_Parse(capsuleText.c_str());
      cJSON *processing = cJSON_Parse(processingText.c_str());
      const char *metadataId = capsule == nullptr ? nullptr :
          jsonString(capsule, "id");
      const char *processingId = processing == nullptr ? nullptr :
          jsonString(processing, "capsuleId");
      const char *audioFile = processing == nullptr ? nullptr :
          jsonString(processing, "audioFile");
      const int64_t schema = processing == nullptr ? -1 :
          jsonInt64(processing, "schemaVersion");
      const bool valid = stagedPath != nullptr &&
          String(stagedPath) == expected && !targetFolder.isEmpty() &&
          sameUuid(id, metadataId) && sameUuid(id, processingId) &&
          (schema == 1 || schema == 2) && safeCapsuleFileName(audioFile) &&
          storageExists(source + "/" + audioFile, StorageAccess::read) &&
          storageExists(targetFolder, StorageAccess::read) &&
          !storageExists(target, StorageAccess::read);
      cJSON_Delete(capsule);
      cJSON_Delete(processing);
      if (!valid) {
        message = "invalid staged import";
        return false;
      }
      strlcpy(plan.source, source.c_str(), sizeof(plan.source));
      strlcpy(plan.target, target.c_str(), sizeof(plan.target));
    }
  }
  sealCapsuleBatchPlan(plan);
  if (!validCapsuleBatchPlan(plan, operation)) {
    message = "command plan is invalid";
    return false;
  }
  return true;
}

bool PokePodLinkService::startBatchPreflight(size_t index) {
  String message;
  if (!buildBatchPlan(index, batchPlan_, message) ||
      !batchJournalStore_.writePlan(batchJournalState_,
                                    static_cast<uint16_t>(index), batchPlan_,
                                    storageOwner())) {
    batchMessage_ = message.isEmpty() ? "batch preflight failed" : message;
    return false;
  }
  if ((batchPlan_.flags & kBatchPlanCapsuleBackup) != 0) {
    const bool started = batchTreeStepper_.beginCopy(
        *fs_, String(batchPlan_.source) + "/capsule.json",
        batchArtifactPath(index, ".capsule.bak"), storageOwner(),
        [](void *context) {
          return static_cast<PokePodLinkService *>(context)
              ->batchForegroundPermitted();
        }, this);
    if (!started) return false;
    batchPending_ = BatchPending::backupCapsule;
    return true;
  }
  if ((batchPlan_.flags & kBatchPlanTrashBackup) != 0) {
    if (!batchTreeStepper_.beginCopy(
            *fs_, String(batchPlan_.source) + "/trash.json",
            batchArtifactPath(index, ".trash.bak"), storageOwner(),
            [](void *context) {
              return static_cast<PokePodLinkService *>(context)
                  ->batchForegroundPermitted();
            }, this)) return false;
    batchPending_ = BatchPending::backupTrash;
    return true;
  }
  if ((batchPlan_.flags & kBatchPlanProcessingBackup) != 0) {
    if (!batchTreeStepper_.beginCopy(
            *fs_, String(batchPlan_.source) + "/processing.json",
            batchArtifactPath(index, ".processing.bak"), storageOwner(),
            [](void *context) {
              return static_cast<PokePodLinkService *>(context)
                  ->batchForegroundPermitted();
            }, this)) return false;
    batchPending_ = BatchPending::backupProcessing;
    return true;
  }
  finishBatchWork(true);
  return true;
}

bool PokePodLinkService::buildBatchMetadata(
    const StoredCapsuleBatchPlan &plan, String &value, String &message) {
  cJSON *command = asJson(batchJsonRoot_);
  const char *operation = batchJournalState_.operation;
  batchMetadataSecondValue_ = "";
  if (strcmp(operation, "deleteCapsules") == 0) {
    const String source(plan.source);
    const int slash = source.lastIndexOf('/');
    const int folderStart = strlen(kCapsuleRoot) + 1;
    String folder = slash <= folderStart ? "Inbox" :
        source.substring(folderStart, slash);
    cJSON *trash = cJSON_CreateObject();
    cJSON_AddNumberToObject(trash, "schemaVersion", 1);
    cJSON_AddStringToObject(trash, "capsuleId", plan.id);
    cJSON_AddStringToObject(trash, "trashedAt", board_->utcNow().c_str());
    cJSON_AddStringToObject(trash, "originalFolder", folder.c_str());
    cJSON_AddNumberToObject(trash, "revision", plan.expectedRevision + 1);
    value = printed(trash) + "\n";
    cJSON_Delete(trash);
    return !value.isEmpty();
  }

  const bool copy = strcmp(operation, "copyCapsules") == 0;
  const String directory = copy ? String(plan.target) :
      ((strcmp(operation, "moveCapsules") == 0 ||
        strcmp(operation, "restoreCapsules") == 0 ||
        strcmp(operation, "deleteFolderToInbox") == 0)
           ? String(plan.target) : String(plan.source));
  cJSON *capsule = cJSON_Parse(
      readText(directory + "/capsule.json", 8192).c_str());
  cJSON *revision = capsule == nullptr ? nullptr :
      cJSON_GetObjectItemCaseSensitive(capsule, "revision");
  if (capsule == nullptr || !cJSON_IsNumber(revision)) {
    cJSON_Delete(capsule);
    message = "capsule metadata is malformed";
    return false;
  }
  if (copy) {
    replaceStringOrNull(capsule, "id", plan.targetId);
    replaceStringOrNull(capsule, "createdAt", board_->utcNow().c_str());
    cJSON_SetNumberValue(revision, 1);
  } else if (strcmp(operation, "setFavorite") == 0) {
    cJSON *favorite = cJSON_GetObjectItemCaseSensitive(command, "favorite");
    if (!cJSON_IsBool(favorite)) {
      cJSON_Delete(capsule);
      message = "favorite value is required";
      return false;
    }
    cJSON *replacement = cJSON_CreateBool(cJSON_IsTrue(favorite));
    if (cJSON_HasObjectItem(capsule, "favorite")) {
      cJSON_ReplaceItemInObjectCaseSensitive(capsule, "favorite", replacement);
    } else {
      cJSON_AddItemToObject(capsule, "favorite", replacement);
    }
    cJSON_SetNumberValue(revision, revision->valueint + 1);
  } else if (strcmp(operation, "addTags") == 0 ||
             strcmp(operation, "removeTags") == 0 ||
             strcmp(operation, "renameTag") == 0 ||
             strcmp(operation, "mergeTag") == 0 ||
             strcmp(operation, "deleteTag") == 0) {
    cJSON *requested = cJSON_GetObjectItemCaseSensitive(command, "tags");
    if (!cJSON_IsArray(requested) || cJSON_GetArraySize(requested) > 32) {
      cJSON_Delete(capsule);
      message = "invalid tags";
      return false;
    }
    std::vector<String> tags;
    std::vector<String> values;
    cJSON *tag = nullptr;
    cJSON_ArrayForEach(tag, requested) {
      const char *text = cJSON_GetStringValue(tag);
      if (text == nullptr || strlen(text) > 64) {
        cJSON_Delete(capsule);
        message = "invalid tags";
        return false;
      }
      tags.emplace_back(text);
    }
    cJSON *old = cJSON_GetObjectItemCaseSensitive(capsule, "tags");
    cJSON_ArrayForEach(tag, old) {
      const char *text = cJSON_GetStringValue(tag);
      if (text != nullptr) values.emplace_back(text);
    }
    if (strcmp(operation, "addTags") == 0) {
      for (const String &requestedTag : tags) {
        bool found = false;
        for (const String &existing : values) {
          if (existing.equalsIgnoreCase(requestedTag)) found = true;
        }
        if (!found) values.push_back(requestedTag);
      }
    } else if (strcmp(operation, "removeTags") == 0 ||
               strcmp(operation, "deleteTag") == 0) {
      values.erase(std::remove_if(values.begin(), values.end(),
          [&](const String &existing) {
            for (const String &requestedTag : tags) {
              if (existing.equalsIgnoreCase(requestedTag)) return true;
            }
            return false;
          }), values.end());
    } else {
      if (tags.size() != 2) {
        cJSON_Delete(capsule);
        message = "tag rename needs old and new names";
        return false;
      }
      for (String &existing : values) {
        if (existing.equalsIgnoreCase(tags[0])) existing = tags[1];
      }
      for (size_t left = 0; left < values.size(); ++left) {
        values.erase(std::remove_if(values.begin() + left + 1, values.end(),
            [&](const String &other) {
              return other.equalsIgnoreCase(values[left]);
            }), values.end());
      }
    }
    cJSON *replacement = cJSON_CreateArray();
    for (const String &existing : values) {
      cJSON_AddItemToArray(replacement,
                          cJSON_CreateString(existing.c_str()));
    }
    if (cJSON_HasObjectItem(capsule, "tags")) {
      cJSON_ReplaceItemInObjectCaseSensitive(capsule, "tags", replacement);
    } else {
      cJSON_AddItemToObject(capsule, "tags", replacement);
    }
    cJSON_SetNumberValue(revision, revision->valueint + 1);
  } else {
    cJSON_SetNumberValue(revision, revision->valueint + 1);
  }
  replaceStringOrNull(capsule, "updatedAt", board_->utcNow().c_str());
  value = printed(capsule) + "\n";
  cJSON_Delete(capsule);
  if (!copy) return !value.isEmpty();

  cJSON *processing = cJSON_Parse(
      readText(directory + "/processing.json", 8192).c_str());
  cJSON *processingRevision = processing == nullptr ? nullptr :
      cJSON_GetObjectItemCaseSensitive(processing, "revision");
  if (processing == nullptr || !cJSON_IsNumber(processingRevision)) {
    cJSON_Delete(processing);
    message = "processing metadata is malformed";
    return false;
  }
  replaceStringOrNull(processing, "capsuleId", plan.targetId);
  cJSON_SetNumberValue(processingRevision,
                       processingRevision->valueint + 1);
  batchMetadataSecondValue_ = printed(processing) + "\n";
  cJSON_Delete(processing);
  return !value.isEmpty() && !batchMetadataSecondValue_.isEmpty();
}

bool PokePodLinkService::startBatchMetadataCommit(
    const StoredCapsuleBatchPlan &plan, bool rollback) {
  const String key = batchTransactionId_ + "-" +
      String(static_cast<unsigned>(batchWork_.index)) +
      (rollback ? "-rollback" : "-apply");
  if (rollback) {
    CapsuleTransactionInput inputs[2];
    uint8_t count = 0;
    const String capsuleBackup = batchArtifactPath(
        batchWork_.index, ".capsule.bak");
    const String trashBackup = batchArtifactPath(
        batchWork_.index, ".trash.bak");
    if ((plan.flags & kBatchPlanCapsuleBackup) != 0 &&
        storageExists(capsuleBackup, StorageAccess::read)) {
      inputs[count++] = {String(plan.source) + "/capsule.json", nullptr,
                         capsuleBackup};
    }
    if ((plan.flags & kBatchPlanTrashBackup) != 0 &&
        storageExists(trashBackup, StorageAccess::read)) {
      inputs[count++] = {String(plan.source) + "/trash.json", nullptr,
                         trashBackup};
    }
    const String processingBackup = batchArtifactPath(
        batchWork_.index, ".processing.bak");
    if ((plan.flags & kBatchPlanProcessingBackup) != 0 &&
        storageExists(processingBackup, StorageAccess::read)) {
      inputs[count++] = {String(plan.source) + "/processing.json", nullptr,
                         processingBackup};
    }
    if (count == 0) return true;
    if (!transactionRunner_.startCommit(key.c_str(), inputs, count,
                                        storageOwner())) return false;
    batchPending_ = BatchPending::rollbackMetadata;
    return true;
  }
  String message;
  if (!buildBatchMetadata(plan, batchMetadataValue_, message)) {
    batchMessage_ = message;
    return false;
  }
  batchByteSource_.bind(batchMetadataValue_);
  CapsuleTransactionInput inputs[2];
  uint8_t count = 1;
  String firstTarget;
  if (strcmp(batchJournalState_.operation, "deleteCapsules") == 0) {
    firstTarget = String(plan.source) + "/trash.json";
  } else {
    const bool copied = strcmp(batchJournalState_.operation,
                               "copyCapsules") == 0;
    firstTarget = String(copied ? plan.target :
        ((strcmp(batchJournalState_.operation, "moveCapsules") == 0 ||
          strcmp(batchJournalState_.operation, "restoreCapsules") == 0 ||
          strcmp(batchJournalState_.operation, "deleteFolderToInbox") == 0)
             ? plan.target : plan.source)) + "/capsule.json";
  }
  inputs[0] = {firstTarget, &batchByteSource_, String()};
  if (strcmp(batchJournalState_.operation, "copyCapsules") == 0) {
    batchSecondByteSource_.bind(batchMetadataSecondValue_);
    inputs[1] = {String(plan.target) + "/processing.json",
                 &batchSecondByteSource_, String()};
    count = 2;
  }
  if (!transactionRunner_.startCommit(key.c_str(), inputs, count,
                                      storageOwner())) return false;
  batchPending_ = BatchPending::applyMetadata;
  return true;
}

bool PokePodLinkService::startBatchApply(size_t index) {
  if (!batchJournalStore_.readPlan(batchJournalState_,
                                   static_cast<uint16_t>(index), batchPlan_,
                                   storageOwner())) {
    batchMessage_ = "cannot read durable batch plan";
    return false;
  }
  if ((batchPlan_.flags & kBatchPlanNoOp) != 0) {
    finishBatchWork(true);
    return true;
  }
  const char *operation = batchJournalState_.operation;
  if (strcmp(operation, "beginMaintenance") == 0 ||
      strcmp(operation, "endMaintenance") == 0 ||
      strcmp(operation, "rescan") == 0) {
    finishBatchWork(true);
    return true;
  }
  if (strcmp(operation, "createFolder") == 0) {
    finishBatchWork(storageMkdir(batchPlan_.source));
    return true;
  }
  if (strcmp(operation, "renameFolder") == 0 ||
      strcmp(operation, "commitImport") == 0) {
    finishBatchWork(storageRename(batchPlan_.source, batchPlan_.target));
    return true;
  }
  if (strcmp(operation, "requeueTranscription") == 0) {
    finishBatchWork(library_->requeue(batchPlan_.id));
    return true;
  }
  if (strcmp(operation, "copyCapsules") == 0) {
    if (!batchTreeStepper_.beginCopy(
            *fs_, batchPlan_.source, batchPlan_.target, storageOwner(),
            [](void *context) {
              return static_cast<PokePodLinkService *>(context)
                  ->batchForegroundPermitted();
            }, this)) return false;
    batchPending_ = BatchPending::applyTree;
    batchPendingStep_ = 0;
    return true;
  }
  if (strcmp(operation, "purgeCapsules") == 0) {
    const String staging = batchPurgePath(batchPlan_);
    if (storageExists(staging, StorageAccess::read) ||
        !storageRename(batchPlan_.source, staging)) {
      batchMessage_ = "purge staging failed";
      return false;
    }
    finishBatchWork(true);
    return true;
  }
  if (strcmp(operation, "deleteCapsules") == 0) {
    batchPendingStep_ = 0;
    return startBatchMetadataCommit(batchPlan_, false);
  }
  if (strcmp(operation, "setFavorite") == 0 ||
      strcmp(operation, "addTags") == 0 ||
      strcmp(operation, "removeTags") == 0 ||
      strcmp(operation, "renameTag") == 0 ||
      strcmp(operation, "mergeTag") == 0 ||
      strcmp(operation, "deleteTag") == 0) {
    batchPendingStep_ = 2;
    return startBatchMetadataCommit(batchPlan_, false);
  }
  if (!storageRename(batchPlan_.source, batchPlan_.target)) {
    batchMessage_ = "capsule move failed";
    return false;
  }
  // Directory rename is one atomic primitive. Metadata publication starts on
  // a later poll so the current poll never combines two storage mutations.
  batchPending_ = BatchPending::applyPath;
  batchPendingStep_ = strcmp(operation, "restoreCapsules") == 0 ? 0 : 1;
  return true;
}

bool PokePodLinkService::startBatchRollback(size_t index) {
  if (!batchJournalStore_.readPlan(batchJournalState_,
                                   static_cast<uint16_t>(index), batchPlan_,
                                   storageOwner())) {
    batchMessage_ = "cannot read rollback plan";
    return false;
  }
  if ((batchPlan_.flags & kBatchPlanNoOp) != 0) {
    finishBatchWork(true);
    return true;
  }
  const char *operation = batchJournalState_.operation;
  if (strcmp(operation, "beginMaintenance") == 0 ||
      strcmp(operation, "endMaintenance") == 0 ||
      strcmp(operation, "rescan") == 0) {
    finishBatchWork(true);
    return true;
  }
  if (strcmp(operation, "createFolder") == 0) {
    const bool ok = !storageExists(batchPlan_.source, StorageAccess::read) ||
        storageRmdir(batchPlan_.source);
    finishBatchWork(ok);
    return true;
  }
  if (strcmp(operation, "renameFolder") == 0 ||
      strcmp(operation, "commitImport") == 0) {
    const bool sourceExists = storageExists(
        batchPlan_.source, StorageAccess::read);
    const bool targetExists = storageExists(
        batchPlan_.target, StorageAccess::read);
    const bool ok = (sourceExists && !targetExists) ||
        (!sourceExists && targetExists &&
         storageRename(batchPlan_.target, batchPlan_.source));
    finishBatchWork(ok);
    return true;
  }
  if (strcmp(operation, "requeueTranscription") == 0) {
    return startBatchMetadataCommit(batchPlan_, true);
  }
  if (strcmp(operation, "deleteFolderToInbox") == 0 &&
      !ensureDirectoryTree(parentPath(batchPlan_.source))) {
    batchMessage_ = "folder rollback path failed";
    return false;
  }
  if (strcmp(operation, "copyCapsules") == 0) {
    if (!storageExists(batchPlan_.target, StorageAccess::read)) {
      finishBatchWork(true);
      return true;
    }
    if (!batchTreeStepper_.beginRemove(*fs_, batchPlan_.target,
                                       storageOwner())) return false;
    batchPending_ = BatchPending::rollbackTree;
    return true;
  }
  if (strcmp(operation, "purgeCapsules") == 0) {
    const String staging = batchPurgePath(batchPlan_);
    const bool ok = !storageExists(staging, StorageAccess::read) ||
        storageRename(staging, batchPlan_.source);
    finishBatchWork(ok);
    return true;
  }
  if (strcmp(operation, "deleteCapsules") == 0) {
    if (storageExists(batchPlan_.target, StorageAccess::read) &&
        !storageExists(batchPlan_.source, StorageAccess::read) &&
        !storageRename(batchPlan_.target, batchPlan_.source)) return false;
    const bool ok = storageRemove(String(batchPlan_.source) + "/trash.json");
    finishBatchWork(ok);
    return true;
  }
  const bool metadata = strcmp(operation, "setFavorite") == 0 ||
      strcmp(operation, "addTags") == 0 ||
      strcmp(operation, "removeTags") == 0 ||
      strcmp(operation, "renameTag") == 0 ||
      strcmp(operation, "mergeTag") == 0 ||
      strcmp(operation, "deleteTag") == 0;
  if (!metadata && storageExists(batchPlan_.target, StorageAccess::read) &&
      !storageExists(batchPlan_.source, StorageAccess::read) &&
      !storageRename(batchPlan_.target, batchPlan_.source)) return false;
  return startBatchMetadataCommit(batchPlan_, true);
}

bool PokePodLinkService::startBatchFinalize() {
  if (strcmp(batchJournalState_.operation, "deleteFolderToInbox") != 0) {
    finishBatchWork(true);
    return true;
  }
  const char *folder = batchJournalState_.finalizeFolder;
  if (!safeFolder(folder, false)) {
    batchMessage_ = "invalid folder delete";
    return false;
  }
  const String source = String(kCapsuleRoot) + "/" + folder;
  if (!batchTreeStepper_.beginVerifyAbsent(
          *fs_, source, "*", storageOwner(),
          [](void *context) {
            return static_cast<PokePodLinkService *>(context)
                ->batchForegroundPermitted();
          }, this)) return false;
  batchPending_ = BatchPending::finalizeFolder;
  batchPendingStep_ = 0;
  return true;
}

bool PokePodLinkService::startBatchRollbackFinalize() {
  if (strcmp(batchJournalState_.operation, "deleteFolderToInbox") != 0 ||
      !safeFolder(batchJournalState_.finalizeFolder, false)) {
    finishBatchWork(false);
    return true;
  }
  const String source = String(kCapsuleRoot) + "/" +
      batchJournalState_.finalizeFolder;
  const String staging = batchFolderStagingPath();
  const bool sourceExists = storageExists(source, StorageAccess::read);
  const bool stagingExists = storageExists(staging, StorageAccess::read);
  const bool ok = (sourceExists && !stagingExists) ||
      (!sourceExists && stagingExists &&
       ensureDirectoryTree(parentPath(source)) &&
       storageRename(staging, source));
  finishBatchWork(ok);
  return true;
}

void PokePodLinkService::advanceBatchPending() {
  if (batchPending_ == BatchPending::persistResult) {
    const CapsuleTransactionPollResult result = transactionRunner_.poll(
        millis(), nullptr);
    if (result == CapsuleTransactionPollResult::progress ||
        result == CapsuleTransactionPollResult::wouldBlock) return;
    const bool persisted = result == CapsuleTransactionPollResult::committed;
    if (persisted) applyBatchResultSideEffects();
    finishBatchWork(persisted);
    return;
  }
  if (batchPending_ == BatchPending::backupCapsule ||
      batchPending_ == BatchPending::backupTrash ||
      batchPending_ == BatchPending::backupProcessing ||
      batchPending_ == BatchPending::applyTree ||
      batchPending_ == BatchPending::rollbackTree ||
      batchPending_ == BatchPending::finalizeFolder ||
      batchPending_ == BatchPending::cleanupTree) {
    const BatchPending pending = batchPending_;
    const LinkTreeStepper::Result result = batchTreeStepper_.poll();
    if (result == LinkTreeStepper::Result::progress ||
        result == LinkTreeStepper::Result::wouldBlock) return;
    if (result != LinkTreeStepper::Result::complete) {
      if (pending == BatchPending::finalizeFolder) {
        batchMessage_ = "folder contents changed during preflight";
      }
      finishBatchWork(false);
      return;
    }
    if (pending == BatchPending::finalizeFolder) {
      const String source = String(kCapsuleRoot) + "/" +
          batchJournalState_.finalizeFolder;
      const String staging = batchFolderStagingPath();
      if (batchPendingStep_ != 0 ||
          storageExists(staging, StorageAccess::read) ||
          !storageRename(source, staging)) {
        batchMessage_ = "folder staging failed";
        finishBatchWork(false);
      } else {
        batchPendingStep_ = 1;
        finishBatchWork(true);
      }
      return;
    }
    if (pending == BatchPending::backupCapsule &&
        (batchPlan_.flags & kBatchPlanTrashBackup) != 0) {
      if (!batchTreeStepper_.beginCopy(
              *fs_, String(batchPlan_.source) + "/trash.json",
              batchArtifactPath(batchWork_.index, ".trash.bak"),
              storageOwner(),
              [](void *context) {
                return static_cast<PokePodLinkService *>(context)
                    ->batchForegroundPermitted();
              }, this)) {
        finishBatchWork(false);
        return;
      }
      batchPending_ = BatchPending::backupTrash;
      return;
    }
    if ((pending == BatchPending::backupCapsule ||
         pending == BatchPending::backupTrash) &&
        (batchPlan_.flags & kBatchPlanProcessingBackup) != 0) {
      if (!batchTreeStepper_.beginCopy(
              *fs_, String(batchPlan_.source) + "/processing.json",
              batchArtifactPath(batchWork_.index, ".processing.bak"),
              storageOwner(),
              [](void *context) {
                return static_cast<PokePodLinkService *>(context)
                    ->batchForegroundPermitted();
              }, this)) {
        finishBatchWork(false);
        return;
      }
      batchPending_ = BatchPending::backupProcessing;
      return;
    }
    if (pending == BatchPending::applyTree) {
      batchPendingStep_ = 2;
      if (!startBatchMetadataCommit(batchPlan_, false)) {
        finishBatchWork(false);
      }
      return;
    }
    if (pending == BatchPending::cleanupTree) {
      if (strcmp(batchJournalState_.operation,
                 "deleteFolderToInbox") == 0 &&
          batchPendingStep_ == 4) {
        batchPending_ = BatchPending::cleanupArtifacts;
        return;
      }
      ++batchCleanupIndex_;
      batchPendingStep_ = 0;
      batchPending_ = BatchPending::cleanupArtifacts;
      return;
    }
    finishBatchWork(true);
    return;
  }

  if (batchPending_ == BatchPending::applyPath) {
    if (batchPendingStep_ == 0) {
      if (!storageRemove(String(batchPlan_.target) + "/trash.json")) {
        finishBatchWork(false);
        return;
      }
      batchPendingStep_ = 1;
      return;
    }
    batchPendingStep_ = 2;
    if (!startBatchMetadataCommit(batchPlan_, false)) {
      finishBatchWork(false);
    }
    return;
  }

  if (batchPending_ == BatchPending::applyMetadata ||
      batchPending_ == BatchPending::rollbackMetadata) {
    const bool rollback = batchPending_ == BatchPending::rollbackMetadata;
    const CapsuleTransactionPollResult result = transactionRunner_.poll(
        millis(), rollback ? nullptr : &transactionGate_);
    if (result == CapsuleTransactionPollResult::progress ||
        result == CapsuleTransactionPollResult::wouldBlock) return;
    if ((!rollback && result != CapsuleTransactionPollResult::committed) ||
        (rollback && result != CapsuleTransactionPollResult::committed)) {
      finishBatchWork(false);
      return;
    }
    if (!rollback &&
        strcmp(batchJournalState_.operation, "deleteCapsules") == 0 &&
        batchPendingStep_ == 0) {
      batchPendingStep_ = 1;
      if (!storageRename(batchPlan_.source, batchPlan_.target)) {
        finishBatchWork(false);
      } else {
        finishBatchWork(true);
      }
      return;
    }
    finishBatchWork(true);
    return;
  }

  if (batchPending_ == BatchPending::cleanupArtifacts) {
    if (batchCleanupIndex_ >= batchExecutor_.total()) {
      if (strcmp(batchJournalState_.operation,
                 "deleteFolderToInbox") == 0 &&
          batchExecutor_.success() && batchPendingStep_ != 4) {
        const String staging = batchFolderStagingPath();
        if (storageExists(staging, StorageAccess::read)) {
          if (!batchTreeStepper_.beginRemove(*fs_, staging,
                                              storageOwner())) {
            finishBatchWork(false);
            return;
          }
          batchPendingStep_ = 4;
          batchPending_ = BatchPending::cleanupTree;
          return;
        }
        batchPendingStep_ = 4;
        return;
      }
      if (strcmp(batchJournalState_.operation, "commitImport") == 0 &&
          batchExecutor_.success() && batchPendingStep_ != 5) {
        const String stagingParent = parentPath(batchPlan_.source);
        if (storageExists(stagingParent, StorageAccess::read) &&
            !storageRmdir(stagingParent)) {
          finishBatchWork(false);
          return;
        }
        batchPendingStep_ = 5;
        return;
      }
      const bool erased = batchExecutor_.preserveJournal() ||
          batchJournalStore_.erase(batchTransactionId_, storageOwner());
      finishBatchWork(erased);
      return;
    }
    if (!batchJournalStore_.readPlan(
            batchJournalState_, static_cast<uint16_t>(batchCleanupIndex_),
            batchPlan_, storageOwner())) {
      finishBatchWork(false);
      return;
    }
    if (batchPendingStep_ == 0) {
      if (!storageRemove(batchArtifactPath(
              batchCleanupIndex_, ".capsule.bak"))) {
        finishBatchWork(false);
        return;
      }
      batchPendingStep_ = 1;
      return;
    }
    if (batchPendingStep_ == 1) {
      if (!storageRemove(batchArtifactPath(
              batchCleanupIndex_, ".trash.bak"))) {
        finishBatchWork(false);
        return;
      }
      batchPendingStep_ = 2;
      return;
    }
    if (batchPendingStep_ == 2) {
      if (!storageRemove(batchArtifactPath(
              batchCleanupIndex_, ".processing.bak"))) {
        finishBatchWork(false);
        return;
      }
      batchPendingStep_ = 3;
      return;
    }
    if (strcmp(batchJournalState_.operation, "purgeCapsules") == 0 &&
        batchExecutor_.success()) {
      const String staging = batchPurgePath(batchPlan_);
      if (storageExists(staging, StorageAccess::read)) {
        if (!batchTreeStepper_.beginRemove(*fs_, staging, storageOwner())) {
          finishBatchWork(false);
          return;
        }
        batchPending_ = BatchPending::cleanupTree;
        return;
      }
    }
    ++batchCleanupIndex_;
    batchPendingStep_ = 0;
  }
}

bool PokePodLinkService::startBatchResultPersistence() {
  cJSON *result = cJSON_CreateObject();
  cJSON_AddNumberToObject(result, "schemaVersion", 1);
  cJSON_AddStringToObject(result, "commandId", batchTransactionId_.c_str());
  cJSON_AddStringToObject(result, "transactionId",
                         batchTransactionId_.c_str());
  cJSON_AddBoolToObject(result, "ok", batchExecutor_.success());
  cJSON_AddBoolToObject(result, "success", batchExecutor_.success());
  const bool queued = batchExecutor_.success() &&
      (strcmp(batchJournalState_.operation, "rescan") == 0 ||
       strcmp(batchJournalState_.operation, "requeueTranscription") == 0);
  cJSON_AddStringToObject(result, "message",
      batchExecutor_.success() ? (queued ? "queued" : "committed") :
      (batchMessage_.isEmpty() ? "batch command failed" :
                                 batchMessage_.c_str()));
  cJSON_AddStringToObject(result, "completedAt", board_->utcNow().c_str());
  batchResultValue_ = printed(result) + "\n";
  cJSON_Delete(result);
  const String path = String(kCapsuleSystem) + "/commands/results/" +
      batchTransactionId_ + ".json";
  batchByteSource_.bind(batchResultValue_);
  const CapsuleTransactionInput input{path, &batchByteSource_, String()};
  const String key = batchTransactionId_ + "-result";
  if (!transactionRunner_.startCommit(key.c_str(), &input, 1,
                                      storageOwner())) return false;
  batchPending_ = BatchPending::persistResult;
  return true;
}

void PokePodLinkService::applyBatchResultSideEffects() {
  if (!batchExecutor_.success()) return;
  if (batchPlan_.id[0] == '\0' && batchExecutor_.total() != 0) {
    if (!batchJournalStore_.readPlan(batchJournalState_, 0, batchPlan_,
                                     storageOwner())) return;
  }
  applyDurableCommandSideEffects(batchJournalState_.operation,
                                 batchPlan_.targetId);
}

void PokePodLinkService::applyDurableCommandSideEffects(
    const char *operation, const char *targetId) {
  if (operation == nullptr) return;
  if (strcmp(operation, "beginMaintenance") == 0 && isUuid(targetId)) {
    const bool changed = activeMaintenance_.isEmpty();
    if (changed || activeMaintenance_.equalsIgnoreCase(targetId)) {
      activeMaintenance_ = targetId;
      if (changed) maintenanceCompletion_.beginAccepted();
    }
  } else if (strcmp(operation, "endMaintenance") == 0 && isUuid(targetId)) {
    if (activeMaintenance_.isEmpty() ||
        activeMaintenance_.equalsIgnoreCase(targetId)) {
      activeMaintenance_ = "";
      maintenanceCompletion_.endResultPersisted(targetId);
    }
  } else if (strcmp(operation, "requeueTranscription") == 0) {
    tencent_->wake();
  } else if (strcmp(operation, "rescan") == 0) {
    (void)library_->requestScan();
  }
}

void PokePodLinkService::applyCompletedCommandSideEffects(void *jsonRoot) {
  cJSON *root = asJson(jsonRoot);
  if (root == nullptr || !cJSON_IsObject(root) ||
      jsonInt64(root, "schemaVersion") != 2 ||
      !sameUuid(jsonString(root, "transactionId"),
                commandLoadTransactionId_.c_str())) return;
  const char *operation = jsonString(root, "operation");
  const char *targetId = jsonString(root, "maintenanceId");
  applyDurableCommandSideEffects(operation, targetId);
}

bool PokePodLinkService::cleanupBatchArtifacts() {
  // Preserve an authority whose durable rollback checkpoint could not be
  // advanced. Boot recovery will retry it before accepting another mutation.
  if (batchExecutor_.preserveJournal()) return true;
  if (!storageRemove(batchCommandPath_)) return false;
  batchCleanupIndex_ = 0;
  batchPendingStep_ = 0;
  batchPending_ = BatchPending::cleanupArtifacts;
  return true;
}

void PokePodLinkService::finishBatchCommand() {
  const uint32_t requestId = batchRequestId_;
  const bool respond = batchExecutor_.responseAllowed() && sessionActive_ &&
      transferPermitted();
  if (batchExecutor_.preserveJournal() && batchRequestId_ == 0) {
    // Runtime IO/checkpoint failures do not prove corruption. Keep the
    // journal in place and fail closed so a later boot cannot replay an old
    // rollback over newer user data.
    startupRecoveryFailed_ = true;
  }
  if (batchExecutor_.preserveJournal() && batchRequestId_ != 0) {
    mutationRecoveryBlocked_ = true;
  }
  cJSON_Delete(asJson(batchJsonRoot_));
  batchJsonRoot_ = nullptr;
  batchRequestId_ = 0;
  batchCommandPath_ = "";
  batchTransactionId_ = "";
  batchMetadataValue_ = "";
  batchMetadataSecondValue_ = "";
  batchResultValue_ = "";
  batchMessage_ = "";
  batchPending_ = BatchPending::none;
  batchPlan_ = {};
  batchNextIdItem_ = nullptr;
  batchNextIdIndex_ = 0;
  batchJournalState_ = {};
  transactionGate_.reset();
  commandStorageActive_ = false;
  commandStorageReservation_.release();
  operation_.releaseResource(LinkOperationResource::transaction);
  operation_.releaseResource(LinkOperationResource::storageReservation);
  (void)library_->requestScan();
  if (respond) {
    sendOk(requestId, "\"accepted\":true");
  }
}

void PokePodLinkService::abandonBatchCommand() {
  batchExecutor_.disconnect();
  transactionGate_.cancel();
}

bool PokePodLinkService::batchForegroundPermitted() const {
  if (batchExecutor_.phase() == LinkCommandExecutor::Phase::preflight ||
      batchExecutor_.phase() == LinkCommandExecutor::Phase::apply ||
      batchExecutor_.phase() == LinkCommandExecutor::Phase::finalize) {
    return sessionActive_ && transferPermitted();
  }
  return true;
}

void PokePodLinkService::finishCommandStorageCleanup() {
  if (!commandCleanupPending_ || !deferredCommandFiles_.empty() ||
      !deferredTreeCleanupStack_.empty()) return;
  const uint32_t requestId = commandCleanupRequestId_;
  if (deferredCommandFileFailed_ && log_ != nullptr) {
    log_->println("{\"event\":\"link_command_flush_failed\"}");
  }
  deferredCommandFileFailed_ = false;
  commandCleanupPending_ = false;
  commandCleanupRequestId_ = 0;
  commandStorageActive_ = false;
  commandStorageReservation_.release();
  operation_.releaseResource(LinkOperationResource::transaction);
  operation_.releaseResource(LinkOperationResource::storageReservation);
  if (requestId != 0 && sessionActive_ && transferPermitted()) {
    sendOk(requestId, "\"accepted\":true");
  }
}

bool PokePodLinkService::safeFolder(const char *value,
                                    bool allowBuiltIn) const {
  if (value == nullptr) return false;
  if (allowBuiltIn && (strcmp(value, "Inbox") == 0 ||
                       strcmp(value, "Archive") == 0)) return true;
  const size_t length = strlen(value);
  if (length == 0 || length > 161 || value[0] == '.' || value[0] == '/' ||
      value[length - 1] == '/') return false;
  uint8_t segments = 1;
  size_t segmentLength = 0;
  for (size_t index = 0; index < length; ++index) {
    const unsigned char c = static_cast<unsigned char>(value[index]);
    if (c == '\\' || c < 0x20) return false;
    if (c == '/') {
      if (segmentLength == 0 || ++segments > 2 || value[index + 1] == '.') return false;
      segmentLength = 0;
    } else {
      ++segmentLength;
    }
  }
  return segmentLength > 0;
}

String PokePodLinkService::folderDirectory(const char *value) const {
  if (!safeFolder(value)) return String();
  if (strcmp(value, "Inbox") == 0) return String(kCapsuleInbox);
  if (strcmp(value, "Archive") == 0) return String(kCapsuleArchive);
  return String(kCapsuleRoot) + "/" + value;
}

String PokePodLinkService::activeCapsuleDirectory(const String &id) const {
  const CapsuleSummary *record = library_->find(id);
  return record == nullptr || record->readOnly ? String() : record->directory;
}

void PokePodLinkService::queueDeferredTreeCleanup(const String &path) {
  if (path.isEmpty()) return;
  for (const String &queued : deferredTreeCleanupStack_) {
    if (queued == path) return;
  }
  if (deferredTreeCleanupStack_.empty()) deferredTreeCleanupFailures_ = 0;
  deferredTreeCleanupStack_.push_back(path);
}

bool PokePodLinkService::stepDeferredTreeCleanup() {
  if (deferredTreeCleanupStack_.empty() || fs_ == nullptr) return true;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      commandStorageActive_ ? StorageOwner::capsuleTransaction
                            : StorageOwner::recovery,
      StorageAccess::mutation, 0);
  if (!lease) return false;
  const String path = deferredTreeCleanupStack_.back();
  File root = fs_->open(path);
  if (!root) {
    deferredTreeCleanupStack_.pop_back();
    return deferredTreeCleanupStack_.empty();
  }
  if (!root.isDirectory()) {
    root.close();
    if (fs_->remove(path)) {
      deferredTreeCleanupFailures_ = 0;
      deferredTreeCleanupStack_.pop_back();
    } else if (++deferredTreeCleanupFailures_ >= 3) {
      deferredTreeCleanupBlocked_ = true;
      deferredTreeCleanupStack_.clear();
      if (log_ != nullptr) {
        log_->println("{\"event\":\"link_tree_cleanup_blocked\"}");
      }
    }
    return deferredTreeCleanupStack_.empty();
  }
  File entry = root.openNextFile();
  if (!entry) {
    root.close();
    if (fs_->rmdir(path)) {
      deferredTreeCleanupFailures_ = 0;
      deferredTreeCleanupStack_.pop_back();
    } else if (++deferredTreeCleanupFailures_ >= 3) {
      deferredTreeCleanupBlocked_ = true;
      deferredTreeCleanupStack_.clear();
      if (log_ != nullptr) {
        log_->println("{\"event\":\"link_tree_cleanup_blocked\"}");
      }
    }
    return deferredTreeCleanupStack_.empty();
  }
  const String full = entry.name();
  entry.close();
  root.close();
  const int slash = full.lastIndexOf('/');
  const String name = slash >= 0 ? full.substring(slash + 1) : full;
  deferredTreeCleanupStack_.push_back(path + "/" + name);
  return false;
}

bool PokePodLinkService::finishCopiedFiles(File &input, File &output) const {
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, 0);
  if (lease) {
    bool ok = true;
    if (output) {
      output.flush();
      ok = output.getWriteError() == 0;
      output.close();
    }
    if (input) input.close();
    return ok;
  }
  deferStorageFile(output, StorageAccess::mutation, true);
  deferStorageFile(input, StorageAccess::read);
  return false;
}

void PokePodLinkService::deferStorageFile(File &file, StorageAccess access,
                                           bool flushBeforeClose) const {
  if (!file) return;
  deferredCommandFiles_.push_back(
      {file, storageOwner(), access, flushBeforeClose});
  // The queued copy owns the underlying handle. Clearing this reference does
  // not close it; the last reference is released under a later physical lease.
  file = File();
}

bool PokePodLinkService::stepDeferredFileCleanup() {
  if (deferredCommandFiles_.empty()) return true;
  DeferredCommandFile &pending = deferredCommandFiles_.front();
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      pending.owner, pending.access, 0);
  if (!lease) return false;
  if (pending.file) {
    if (pending.flushBeforeClose) {
      pending.file.flush();
      if (pending.file.getWriteError() != 0) {
        deferredCommandFileFailed_ = true;
      }
    }
    pending.file.close();
  }
  deferredCommandFiles_.erase(deferredCommandFiles_.begin());
  return deferredCommandFiles_.empty();
}

}  // namespace pokepod
