#include "PokePodLinkService.h"

#include <esp_heap_caps.h>
#include <esp_system.h>

#include "AudioPipeline.h"
#include "AudioCaptureRuntime.h"
#include "AudioCaptureDispatcher.h"
#include "AudioCaptureRouter.h"
#include "BoardServices.h"
#include "CapsuleLibrary.h"
#include "CapsulePolicy.h"
#include "LinkCommandBatchPolicy.h"
#include "DeviceConfig.h"
#include "Dashboard.h"
#include "FontPolicy.h"
#include "TencentWorker.h"
#include "UsbLinkBridge.h"
#include "BleVoiceService.h"
#include "ProvisioningDiagnostics.h"
#include "PowerDiagnostics.h"
#include "ProvisioningCoordinator.h"
#include "RuntimePowerManager.h"
#include "WavRecorder.h"
#include "WifiController.h"
#include "WirelessSyncIdentityBlob.h"
#include "WirelessSyncPairing.h"
#include "WirelessSyncProtocol.h"

namespace pokepod {
namespace {

constexpr size_t kLinkTxFrameBytes = kLinkHeaderBytes + kLinkMaxDataBytes;
constexpr size_t kLinkPendingControlBytes =
    kLinkHeaderBytes + kLinkMaxControlBytes;
constexpr const char *kLinkCommandUploadPart =
    "/PokeCapsule/.system/commands/incoming/upload.part";
constexpr const char *kLinkStagedUploadPart =
    "/PokeCapsule/.staging/link-upload.part";
constexpr const char *kLinkFontUploadPart =
    "/PokeCapsule/.system/fonts/cjk20.a4.part";

}  // namespace

bool PokePodLinkService::begin(Stream &stream, fs::FS &fs,
                               BoardServices &board, AudioPipeline &audio,
                               AudioCaptureRouter &captureRouter,
                               UsbLinkBridge &usb, BleVoiceService &bleVoice,
                               Dashboard &dashboard,
                               CapsuleLibrary &library, WavRecorder &recorder,
                               DeviceConfig &config, WifiController &wifi,
                               TencentWorker &tencent,
                               ProvisioningDiagnostics &provisioningDiagnostics,
                               PowerDiagnostics &powerDiagnostics,
                               RuntimePowerManager &power, Print &log,
                               LinkServiceCoordinator *coordinator,
                               LinkTransport transport,
                               WirelessSyncPairingProvider *pairingProvider,
                               LinkTransferGate *transferGate,
                               ProvisioningCoordinator *provisioningCoordinator,
                               LinkWriteChannel *writeChannel,
                               AudioCaptureRuntime *captureRuntime,
                               AudioCaptureDispatcher *captureDispatcher,
                               const CapabilityRegistry *capabilities) {
  stream_ = &stream;
  fs_ = &fs;
  board_ = &board;
  audio_ = &audio;
  capabilities_ = capabilities;
  captureRouter_ = &captureRouter;
  usb_ = &usb;
  bleVoice_ = &bleVoice;
  dashboard_ = &dashboard;
  library_ = &library;
  recorder_ = &recorder;
  config_ = &config;
  wifi_ = &wifi;
  tencent_ = &tencent;
  provisioningCoordinator_ = provisioningCoordinator;
  log_ = &log;
  recordingSession_.begin(audio, captureRuntime, captureDispatcher,
                          captureRouter, bleVoice, recorder, library, log);
  diagnostics_.bind(board, audio, usb, bleVoice, dashboard, library, recorder,
                    config, wifi, tencent, provisioningDiagnostics,
                    powerDiagnostics, power, provisioningCoordinator,
                    capabilities);
  fileTransfer_.bind(*this);
  coordinator_ = coordinator;
  transport_ = transport;
  pairingProvider_ = pairingProvider;
  transferGate_ = transferGate;
  writeChannel_ = writeChannel;
  connectionGeneration_ = 0;
  nextConnectionGeneration_ = 0;
  activeMaintenance_ = "";
  quiesceRequested_ = false;
  if (!transaction_.begin(fs, log)) return false;
  if (!transactionRunner_.begin(fs, log)) return false;
  if (!batchJournalStore_.begin(fs)) return false;
  startupPartCleanupIndex_ = 0;
  startupPartCleanupFailures_ = 0;
  startupPartCleanupPending_ = false;
  startupReady_ = false;
  startupRecoveryFailed_ = false;
  startupBatchCandidateInvalid_ = false;
  mutationRecoveryBlocked_ = false;
  startupPurgePending_ = true;
  if (payload_ == nullptr) {
    payload_ = static_cast<uint8_t *>(heap_caps_calloc(
        kLinkMaxDataBytes, sizeof(uint8_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (txFrame_ == nullptr) {
    txFrame_ = static_cast<uint8_t *>(heap_caps_calloc(
        kLinkTxFrameBytes, sizeof(uint8_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (pendingControlFrame_ == nullptr) {
    pendingControlFrame_ = static_cast<uint8_t *>(heap_caps_calloc(
        kLinkPendingControlBytes, sizeof(uint8_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (payload_ == nullptr || txFrame_ == nullptr ||
      pendingControlFrame_ == nullptr) {
    stream_ = nullptr;
    log_->println(
        "{\"event\":\"link_buffer\",\"ok\":false,\"memory\":\"psram\"}");
    return false;
  }
  log_->printf(
      "{\"event\":\"link_buffer\",\"ok\":true,\"memory\":\"psram\",\"bytes\":%u}\n",
      static_cast<unsigned>(kLinkMaxDataBytes + kLinkTxFrameBytes +
                            kLinkPendingControlBytes));
  if (!ensureDirectoryTree(String(kCapsuleSystem) + "/commands/results") ||
      !ensureDirectoryTree(String(kCapsuleSystem) + "/commands/incoming") ||
      !ensureDirectoryTree(CapsuleBatchJournalStore::kDirectory)) {
    return false;
  }
  // Recover durable transactions before sweeping the three fixed Link upload
  // parts. A cut after journal publication but before publishPrepared must let
  // the durable journal choose old/new state before the borrowed part is
  // removed. No transport frames are accepted until both phases finish.
  if (!transactionRunner_.startRecovery(StorageOwner::capsuleTransaction)) {
    return false;
  }
  transactionPurpose_ = TransactionPurpose::startupRecovery;
  return true;
}

bool PokePodLinkService::beginIncoming(IncomingKind kind, uint32_t requestId,
                                       uint32_t expectedBytes,
                                       const String &temporaryPath,
                                       const String &finalPath,
                                       const String &transactionId,
                                       bool chunkAcks) {
  incomingStorageReservation_ = StorageCoordinator::instance().reserve(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  if (!incomingStorageReservation_) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  if (!lease) {
    incomingStorageReservation_.release();
    return false;
  }
  if (fs_->exists(temporaryPath) && !fs_->remove(temporaryPath)) {
    incomingStorageReservation_.release();
    return false;
  }
  File output = fs_->open(temporaryPath, FILE_WRITE);
  if (!output) {
    incomingStorageReservation_.release();
    return false;
  }
  incomingKind_ = kind;
  incomingRequestId_ = requestId;
  incomingExpected_ = expectedBytes;
  incomingReceived_ = 0;
  incomingChunkAcks_ = chunkAcks;
  incomingFile_ = output;
  incomingTemporaryPath_ = temporaryPath;
  incomingFinalPath_ = finalPath;
  incomingTransactionId_ = transactionId;
  incomingLastByteMs_ = millis();
  operation_.advance(LinkOperationState::receiving);
  operation_.ownResource(LinkOperationResource::storageReservation);
  operation_.ownResource(LinkOperationResource::file);
  return true;
}

void PokePodLinkService::processData(uint32_t requestId, uint16_t flags,
                                     const uint8_t *payload, size_t size) {
  if (incomingKind_ == IncomingKind::none || requestId != incomingRequestId_) {
    sendError(requestId, "unexpected data frame");
    return;
  }
  StorageIoLease writeIo = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  if (!writeIo || incomingReceived_ + size > incomingExpected_ ||
      (size > 0 && incomingFile_.write(payload, size) != size)) {
    failIncoming("staged write failed");
    return;
  }
  incomingReceived_ += size;
  incomingLastByteMs_ = millis();
  const bool last = (flags & 1U) != 0;
  if (last && incomingReceived_ != incomingExpected_) {
    failIncoming("binary length mismatch");
    return;
  }
  if (incomingChunkAcks_) {
    sendEvent(requestId, "{\"event\":\"binary_ack\",\"received\":" +
        String(incomingReceived_) + "}");
  }
  if (last) finishIncoming();
}

void PokePodLinkService::finishIncoming() {
  const IncomingKind kind = incomingKind_;
  const uint32_t requestId = incomingRequestId_;
  const String temporary = incomingTemporaryPath_;
  const String finalPath = incomingFinalPath_;
  const String transaction = incomingTransactionId_;
  StorageIoLease finishIo = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  if (!finishIo) {
    failIncoming("storage busy");
    return;
  }
  incomingFile_.flush();
  const bool writeOk = incomingFile_.getWriteError() == 0;
  incomingFile_.close();
  operation_.releaseResource(LinkOperationResource::file);
  incomingKind_ = IncomingKind::none;
  incomingRequestId_ = 0;
  incomingExpected_ = incomingReceived_ = 0;
  incomingChunkAcks_ = false;
  incomingTemporaryPath_ = incomingFinalPath_ = incomingTransactionId_ = "";
  incomingLastByteMs_ = 0;
  const bool valid = writeOk &&
      (kind != IncomingKind::systemFont || validFontFile(temporary));
  finishIo.release();
  const char *key = transaction.isEmpty() ? finalPath.c_str() :
                                            transaction.c_str();
  if (!valid || !transactionRunner_.startPreparedFile(
                    key, temporary, finalPath, storageOwner())) {
    incomingCleanupRespond_ = true;
    incomingCleanupRequestId_ = requestId;
    incomingCleanupMessage_ = "atomic file commit failed";
    if (!incomingCleanup_.begin(incomingFile_, fs_, temporary,
                                incomingStorageReservation_, storageOwner(),
                                StorageAccess::mutation)) return;
    incomingCleanupPending_ = true;
    cleanupIncomingStorage();
    return;
  }
  transactionGate_.beginOperation(transferGate_);
  operation_.advance(LinkOperationState::durableCommit);
  operation_.ownResource(LinkOperationResource::transaction);
  transactionPurpose_ = TransactionPurpose::incoming;
  transactionIncomingKind_ = kind;
  transactionRequestId_ = requestId;
  transactionPreparedPath_ = temporary;
  transactionFinalPath_ = finalPath;
  transactionId_ = transaction;
  transactionRespond_ = sessionActive_ && transferPermitted();
}

void PokePodLinkService::advanceTransactionRunner() {
  if (!transactionRunner_.active() ||
      transactionPurpose_ == TransactionPurpose::none) return;
  CapsuleTransactionGate *gate =
      (transactionPurpose_ == TransactionPurpose::incoming ||
       transactionPurpose_ == TransactionPurpose::commandText)
          ? &transactionGate_ : nullptr;
  const CapsuleTransactionPollResult result = transactionRunner_.poll(
      millis(), gate);
  if (result == CapsuleTransactionPollResult::progress ||
      result == CapsuleTransactionPollResult::wouldBlock) return;
  if (transactionPurpose_ == TransactionPurpose::startupRecovery) {
    finishStartupRecovery(result ==
                          CapsuleTransactionPollResult::recovered);
  } else if (transactionPurpose_ == TransactionPurpose::incoming) {
    finishIncomingTransaction(result ==
                              CapsuleTransactionPollResult::committed);
  } else if (transactionPurpose_ == TransactionPurpose::commandText) {
    finishTextCommand(result == CapsuleTransactionPollResult::committed);
  } else if (transactionPurpose_ == TransactionPurpose::commandTextResult) {
    finishTextResult(result == CapsuleTransactionPollResult::committed);
  } else if (transactionPurpose_ ==
             TransactionPurpose::commandFailureResult) {
    finishCommandFailureResult(
        result == CapsuleTransactionPollResult::committed);
  }
}

void PokePodLinkService::finishStartupRecovery(bool recovered) {
  if (!recovered) {
    startupRecoveryFailed_ = true;
    if (log_ != nullptr) {
      log_->println("{\"event\":\"link_startup_recovery_failed\"}");
    }
    return;
  }
  transactionPurpose_ = TransactionPurpose::none;
  startupBatchRecoveryPending_ = true;
}

void PokePodLinkService::advanceStartupPartCleanup() {
  if (!startupPartCleanupPending_ || startupReady_ ||
      startupRecoveryFailed_) return;
  if (!startupPartCleanupReservation_) {
    startupPartCleanupReservation_ = StorageCoordinator::instance().reserve(
        storageOwner(), StorageAccess::mutation, 0);
    if (!startupPartCleanupReservation_) return;
  }
  static constexpr const char *parts[] = {
      kLinkCommandUploadPart, kLinkStagedUploadPart, kLinkFontUploadPart};
  if (startupPartCleanupIndex_ >= sizeof(parts) / sizeof(parts[0])) {
    startupPartCleanupReservation_.release();
    startupPartCleanupPending_ = false;
    return;
  }
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, 0);
  if (!lease) return;
  const char *path = parts[startupPartCleanupIndex_];
  if (fs_->exists(path) && !fs_->remove(path)) {
    if (++startupPartCleanupFailures_ >= 3) {
      startupRecoveryFailed_ = true;
      startupPartCleanupPending_ = false;
      startupPartCleanupReservation_.release();
      if (log_ != nullptr) {
        log_->println("{\"event\":\"link_startup_part_cleanup_failed\"}");
      }
    }
    return;
  }
  startupPartCleanupFailures_ = 0;
  ++startupPartCleanupIndex_;
}

void PokePodLinkService::finishIncomingTransaction(bool committed) {
  const IncomingKind kind = transactionIncomingKind_;
  const uint32_t requestId = transactionRequestId_;
  const String preparedPath = transactionPreparedPath_;
  const String finalPath = transactionFinalPath_;
  const String transaction = transactionId_;
  const bool respond = transactionRespond_ && sessionActive_ &&
      transferPermitted();
  transactionPurpose_ = TransactionPurpose::none;
  transactionIncomingKind_ = IncomingKind::none;
  transactionRequestId_ = 0;
  transactionPreparedPath_ = "";
  transactionFinalPath_ = "";
  transactionId_ = "";
  transactionRespond_ = false;
  transactionGate_.reset();
  operation_.releaseResource(LinkOperationResource::transaction);
  if (!committed) {
    incomingCleanupRespond_ = respond;
    incomingCleanupRequestId_ = requestId;
    incomingCleanupMessage_ = "atomic file commit failed";
    incomingCleanupPending_ = incomingCleanup_.begin(
        incomingFile_, fs_, preparedPath, incomingStorageReservation_,
        storageOwner(), StorageAccess::mutation);
    if (incomingCleanupPending_) cleanupIncomingStorage();
    return;
  }
  incomingStorageReservation_.release();
  operation_.releaseResource(LinkOperationResource::storageReservation);
  if (kind == IncomingKind::command) {
    handleCommandFile(requestId, finalPath, transaction);
    if (commandCleanupPending_) return;
  } else if (respond && kind == IncomingKind::systemFont) {
    sendOk(requestId, "\"installed\":true,\"rebootRequired\":true");
  } else if (respond) {
    sendOk(requestId);
  }
}

void PokePodLinkService::failIncoming(const char *message) {
  if (!incomingCleanupPending_) {
    incomingCleanupRequestId_ = incomingRequestId_;
    incomingCleanupMessage_ = message == nullptr ? "staged transfer failed" :
                                                    message;
    incomingCleanupRespond_ = sessionActive_ && transferPermitted();
    incomingCleanupPending_ = incomingCleanup_.begin(
        incomingFile_, fs_, incomingTemporaryPath_,
        incomingStorageReservation_, storageOwner(),
        StorageAccess::mutation);
  }
  cleanupIncomingStorage();
}

bool PokePodLinkService::cleanupIncomingStorage() {
  if (!incomingCleanupPending_) return true;
  if (!incomingCleanup_.poll()) return false;
  incomingCleanupPending_ = false;
  finishIncomingCleanup();
  return true;
}

void PokePodLinkService::finishIncomingCleanup() {
  const uint32_t requestId = incomingCleanupRequestId_;
  const String message = incomingCleanupMessage_;
  const bool respond = incomingCleanupRespond_ && sessionActive_ &&
      transferPermitted();
  incomingKind_ = IncomingKind::none;
  incomingRequestId_ = 0;
  incomingExpected_ = incomingReceived_ = 0;
  incomingChunkAcks_ = false;
  incomingTemporaryPath_ = incomingFinalPath_ = incomingTransactionId_ = "";
  incomingLastByteMs_ = 0;
  incomingCleanupRequestId_ = 0;
  incomingCleanupMessage_ = "";
  incomingCleanupRespond_ = false;
  operation_.releaseResource(LinkOperationResource::file);
  operation_.releaseResource(LinkOperationResource::storageReservation);
  operation_.releaseResource(LinkOperationResource::transaction);
  if (respond && requestId != 0) {
    sendError(requestId, message.c_str());
  }
}


bool PokePodLinkService::cleanupPurgeStaging() {
  if (!startupPurgePending_) return true;
  if (fs_ == nullptr) return false;
  if (!startupPurgeReservation_) {
    startupPurgeReservation_ = StorageCoordinator::instance().reserve(
        storageOwner(), StorageAccess::mutation, 0);
    if (!startupPurgeReservation_) return false;
  }
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::read, storageIoTimeout());
  if (!lease) return false;
  if (!startupPurgeDirectory_) {
    startupPurgeDirectory_ = fs_->open(kCapsuleStaging);
  }
  if (!startupPurgeDirectory_ || !startupPurgeDirectory_.isDirectory()) {
    if (startupPurgeDirectory_) startupPurgeDirectory_.close();
    startupPurgeReservation_.release();
    startupPurgePending_ = false;
    return true;
  }
  File entry = startupPurgeDirectory_.openNextFile();
  if (!entry) {
    startupPurgeDirectory_.close();
    startupPurgeReservation_.release();
    startupPurgePending_ = false;
    return true;
  }
  const String full = entry.name();
  const bool isDirectory = entry.isDirectory();
  entry.close();
  const int slash = full.lastIndexOf('/');
  const String name = slash >= 0 ? full.substring(slash + 1) : full;
  if (isDirectory && purgeStagingDirectoryName(name.c_str())) {
    queueDeferredTreeCleanup(String(kCapsuleStaging) + "/" + name);
  }
  return false;
}

String PokePodLinkService::newUuid() const {
  uint8_t bytes[16];
  for (size_t offset = 0; offset < sizeof(bytes); offset += 4) {
    const uint32_t value = esp_random();
    memcpy(bytes + offset, &value, sizeof(value));
  }
  char result[37];
  formatUuidV4(bytes, result);
  return String(result);
}

StorageOwner PokePodLinkService::storageOwner() const {
  if (commandStorageActive_) return StorageOwner::capsuleTransaction;
  return transport_ == LinkTransport::wifi ? StorageOwner::wifiLink
                                            : StorageOwner::usbLink;
}

bool PokePodLinkService::ensureDirectoryTree(const String &path) {
  if (fs_ == nullptr || path.isEmpty() || path[0] != '/') return false;
  String current;
  for (size_t index = 1; index <= path.length(); ++index) {
    if (index == path.length() || path[index] == '/') {
      current = path.substring(0, index);
      if (!storageExists(current, StorageAccess::read) &&
          !storageMkdir(current)) return false;
    }
  }
  return true;
}

bool PokePodLinkService::storageExists(const String &path,
                                       StorageAccess access) const {
  if (fs_ == nullptr) return false;
  const LinkCommandExecutor::Phase phase = batchExecutor_.phase();
  const bool foreground = commandLoadState_ != CommandLoadState::none ||
      phase == LinkCommandExecutor::Phase::preflight ||
      phase == LinkCommandExecutor::Phase::apply ||
      phase == LinkCommandExecutor::Phase::finalize;
  if (foreground && !transferPermitted()) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), access, storageIoTimeout());
  if (!lease) return false;
  const bool exists = fs_->exists(path);
  lease.release();
  return (!foreground || transferPermitted()) && exists;
}

bool PokePodLinkService::storageRename(const String &source,
                                       const String &target) {
  if (fs_ == nullptr) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  return lease && fs_->rename(source, target);
}

bool PokePodLinkService::storageRemove(const String &path) {
  if (fs_ == nullptr) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  return lease && (!fs_->exists(path) || fs_->remove(path));
}

bool PokePodLinkService::storageMkdir(const String &path) {
  if (fs_ == nullptr) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  return lease && (fs_->exists(path) || fs_->mkdir(path));
}

bool PokePodLinkService::storageRmdir(const String &path) {
  if (fs_ == nullptr) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  return lease && (!fs_->exists(path) || fs_->rmdir(path));
}

void PokePodLinkService::closeStorageFile(File &file,
                                          StorageAccess access) const {
  if (!file) return;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), access, 0);
  if (lease) {
    file.close();
    return;
  }
  deferStorageFile(file, access);
}

bool PokePodLinkService::writeTextAtomic(const String &path,
                                         const String &text) {
  return transaction_.writeTextAtomic(
      path, text, storageOwner(), "link-metadata");
}

bool PokePodLinkService::validFontFile(const String &path) const {
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::read, storageIoTimeout());
  if (!lease) return false;
  File file = fs_->open(path, FILE_READ);
  uint8_t header[kFontHeaderBytes];
  if (!file || file.isDirectory() ||
      file.read(header, sizeof(header)) != sizeof(header)) {
    if (file) file.close();
    return false;
  }
  const uint32_t count = linkGet32(header + 12);
  const uint32_t entrySize = linkGet32(header + 16);
  const bool ok = memcmp(header, "PKF2", 4) == 0 &&
      validFontLayout(linkGet16(header + 4), linkGet16(header + 6),
                      header[8], count, entrySize, file.size());
  file.close();
  return ok;
}

String PokePodLinkService::readText(const String &path, size_t limit) const {
  if (fs_ == nullptr) return String();
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::read, storageIoTimeout());
  if (!lease) return String();
  File file = fs_->open(path, FILE_READ);
  if (!file || file.isDirectory() || file.size() > limit) {
    if (file) file.close();
    return String();
  }
  String value;
  if (!value.reserve(file.size() + 1)) {
    file.close();
    return String();
  }
  while (file.available()) value += static_cast<char>(file.read());
  file.close();
  return value;
}

String PokePodLinkService::deviceId() const {
  char value[24];
  formatPokePodDeviceId(ESP.getEfuseMac(), value);
  return String(value);
}

bool PokePodLinkService::foregroundBusy() const {
  if (recorder_ == nullptr || tencent_ == nullptr || bleVoice_ == nullptr ||
      captureRouter_ == nullptr) return true;
  if (audio_ != nullptr && audio_->playing()) return true;
  return linkForegroundBusy(recorder_->recording(), tencent_->working(),
                            bleVoice_->streaming(),
                            captureRouter_->available());
}

}  // namespace pokepod
