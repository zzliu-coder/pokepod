#include "CapsuleLibrary.h"

#include <cJSON.h>
#include <algorithm>

#include "CapsulePolicy.h"

namespace pokepod {
namespace {

constexpr size_t kMaxCapsulesOnDevice = 96;
constexpr size_t kMaxMetadataBytes = 8192;
constexpr size_t kPreviewBytes = 360;

CapsuleStatus parseStatus(const char *value) {
  if (value == nullptr) return CapsuleStatus::damaged;
  if (strcmp(value, "recording") == 0) return CapsuleStatus::recording;
  if (strcmp(value, "recorded") == 0 || strcmp(value, "queued") == 0) {
    return CapsuleStatus::queued;
  }
  if (strcmp(value, "transcribing") == 0) return CapsuleStatus::transcribing;
  if (strcmp(value, "raw_ready") == 0) return CapsuleStatus::rawReady;
  if (strcmp(value, "correcting") == 0) return CapsuleStatus::correcting;
  if (strcmp(value, "ready") == 0) return CapsuleStatus::ready;
  if (strcmp(value, "failed") == 0) return CapsuleStatus::failed;
  return CapsuleStatus::damaged;
}

const char *jsonString(cJSON *root, const char *name) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsString(item) && item->valuestring != nullptr
      ? item->valuestring : nullptr;
}

uint32_t jsonUint(cJSON *root, const char *name) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsNumber(item) && item->valuedouble >= 0
      ? static_cast<uint32_t>(item->valuedouble) : 0;
}

bool jsonBool(cJSON *root, const char *name) {
  return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, name));
}

void replaceStringOrNull(cJSON *root, const char *name, const String &value) {
  cJSON *replacement = value.isEmpty() ? cJSON_CreateNull()
                                        : cJSON_CreateString(value.c_str());
  cJSON_ReplaceItemInObjectCaseSensitive(root, name, replacement);
}

}  // namespace

bool CapsuleLibrary::begin(fs::FS &fs, Print &log) {
  fs_ = &fs;
  log_ = &log;
  if (!scan()) return false;

  std::vector<String> interrupted;
  for (const CapsuleSummary &record : records_) {
    if (capsuleStatusNeedsStartupRequeue(statusName(record.status))) {
      interrupted.push_back(record.id);
    }
  }
  for (const String &id : interrupted) {
    if (log_ != nullptr) {
      log_->printf("{\"event\":\"asr_startup_requeue\",\"capsule_id\":\"%s\"}\n",
                   id.c_str());
    }
    if (!requeue(id)) return false;
  }
  return true;
}

bool CapsuleLibrary::scan() {
  if (fs_ == nullptr) return false;
  records_.clear();
  scanFolder(kCapsuleInbox, "Inbox", 0);
  scanFolder(kCapsuleArchive, "Archive", 0);
  scanFolder(kCapsuleTrash, ".trash", 0);
  File root = fs_->open(kCapsuleRoot);
  if (root && root.isDirectory()) {
    File entry = root.openNextFile();
    while (entry) {
      const String fullName = entry.name();
      const bool directory = entry.isDirectory();
      entry.close();
      const int slash = fullName.lastIndexOf('/');
      const String name = slash >= 0 ? fullName.substring(slash + 1) : fullName;
      if (directory && !name.startsWith(".") && name != "Inbox" &&
          name != "Archive") {
        scanFolder(String(kCapsuleRoot) + "/" + name, name, 1);
      }
      entry = root.openNextFile();
    }
    root.close();
  }
  std::sort(records_.begin(), records_.end(),
            [](const CapsuleSummary &left, const CapsuleSummary &right) {
              return left.createdAt > right.createdAt;
            });
  rebuildVisible();
  if (log_ != nullptr) {
    log_->printf("{\"event\":\"capsule_scan\",\"count\":%u,\"pending\":%u}\n",
                 static_cast<unsigned>(records_.size()),
                 static_cast<unsigned>(pendingCount()));
  }
  return true;
}

void CapsuleLibrary::scanFolder(const String &path, const String &folder,
                                uint8_t depth) {
  if (records_.size() >= kMaxCapsulesOnDevice) return;
  File directory = fs_->open(path);
  if (!directory || !directory.isDirectory()) return;
  File entry = directory.openNextFile();
  while (entry && records_.size() < kMaxCapsulesOnDevice) {
    const String fullName = entry.name();
    const bool isDirectory = entry.isDirectory();
    entry.close();
    const int slash = fullName.lastIndexOf('/');
    const String name = slash >= 0 ? fullName.substring(slash + 1) : fullName;
    if (isDirectory && isUuid(name.c_str())) {
      CapsuleSummary record;
      if (readRecord(path + "/" + name, folder, record)) records_.push_back(record);
    } else if (isDirectory && depth < 2 && !name.startsWith(".")) {
      scanFolder(path + "/" + name, folder + "/" + name, depth + 1);
    }
    entry = directory.openNextFile();
  }
  directory.close();
}

bool CapsuleLibrary::readRecord(const String &directory, const String &folder,
                                CapsuleSummary &record) const {
  const String capsuleText = readText(directory + "/capsule.json", kMaxMetadataBytes);
  const String processingText = readText(directory + "/processing.json", kMaxMetadataBytes);
  if (capsuleText.isEmpty() || processingText.isEmpty()) return false;
  cJSON *capsule = cJSON_ParseWithLength(capsuleText.c_str(), capsuleText.length());
  cJSON *processing = cJSON_ParseWithLength(processingText.c_str(), processingText.length());
  if (capsule == nullptr || processing == nullptr) {
    cJSON_Delete(capsule);
    cJSON_Delete(processing);
    return false;
  }
  const char *id = jsonString(capsule, "id");
  const char *processingId = jsonString(processing, "capsuleId");
  const int slash = directory.lastIndexOf('/');
  const String directoryId = slash >= 0 ? directory.substring(slash + 1) : directory;
  const bool valid = isUuid(id) && processingId != nullptr &&
      strcasecmp(id, processingId) == 0 && directoryId.equalsIgnoreCase(id);
  if (valid) {
    record.id = id;
    record.directory = directory;
    record.folder = folder;
    const char *title = jsonString(capsule, "title");
    const char *createdAt = jsonString(capsule, "createdAt");
    const char *audioFile = jsonString(processing, "audioFile");
    const char *errorStage = jsonString(processing, "errorStage");
    const char *error = jsonString(processing, "error");
    record.title = title == nullptr ? "语音胶囊" : title;
    record.createdAt = createdAt == nullptr ? "" : createdAt;
    record.audioFile = audioFile == nullptr ? "" : audioFile;
    record.errorStage = errorStage == nullptr ? "" : errorStage;
    record.error = error == nullptr ? "" : error;
    record.favorite = jsonBool(capsule, "favorite");
    record.archived = folder == "Archive" || folder.startsWith("Archive/");
    record.trashed = folder == ".trash" || folder.startsWith(".trash/");
    record.durationMs = jsonUint(processing, "durationMs");
    record.status = parseStatus(jsonString(processing, "status"));
    record.preview = readBestText(record, kPreviewBytes);
  }
  cJSON_Delete(capsule);
  cJSON_Delete(processing);
  return valid;
}

size_t CapsuleLibrary::pendingCount() const {
  size_t count = 0;
  for (const CapsuleSummary &record : records_) {
    if (!record.archived && !record.trashed &&
        (record.status == CapsuleStatus::queued ||
         record.status == CapsuleStatus::transcribing)) ++count;
  }
  return count;
}

const CapsuleSummary *CapsuleLibrary::at(size_t index) const {
  return index < visible_.size() ? &records_[visible_[index]] : nullptr;
}

const CapsuleSummary *CapsuleLibrary::nextQueued() const {
  for (const CapsuleSummary &record : records_) {
    if (!record.archived && !record.trashed &&
        record.status == CapsuleStatus::queued) return &record;
  }
  return nullptr;
}

const CapsuleSummary *CapsuleLibrary::find(const String &id) const {
  for (const CapsuleSummary &record : records_) {
    if (record.id.equalsIgnoreCase(id)) return &record;
  }
  return nullptr;
}

bool CapsuleLibrary::markTranscribing(const String &id) {
  return updateProcessing(id, CapsuleStatus::transcribing, "", "", "", true) && scan();
}

bool CapsuleLibrary::commitRawText(const String &id, const String &text) {
  const CapsuleSummary *record = find(id);
  if (record == nullptr || text.isEmpty() ||
      !writeTextAtomic(record->directory + "/raw.txt", text + "\n")) return false;
  return updateProcessing(id, CapsuleStatus::rawReady, "raw.txt", "", "", false) && scan();
}

bool CapsuleLibrary::markFailure(const String &id, const String &stage,
                                 const String &error) {
  return updateProcessing(id, CapsuleStatus::failed, "", stage, error, false) && scan();
}

bool CapsuleLibrary::markRetryable(const String &id, const String &stage,
                                   const String &error) {
  return updateProcessing(id, CapsuleStatus::queued, "", stage, error, false) && scan();
}

bool CapsuleLibrary::requeue(const String &id) {
  return updateProcessing(id, CapsuleStatus::queued, "", "", "", false) && scan();
}

bool CapsuleLibrary::toggleFavorite(const String &id) {
  const CapsuleSummary *record = find(id);
  return record != nullptr && updateFavorite(id, !record->favorite) && scan();
}

bool CapsuleLibrary::archive(const String &id) {
  const CapsuleSummary *record = find(id);
  if (record == nullptr || record->archived || record->trashed ||
      record->status == CapsuleStatus::transcribing ||
      !safeArchiveOriginalFolder(record->folder.c_str())) return false;
  cJSON *metadata = cJSON_CreateObject();
  if (metadata == nullptr) return false;
  cJSON_AddNumberToObject(metadata, "schemaVersion", 1);
  cJSON_AddStringToObject(metadata, "capsuleId", id.c_str());
  cJSON_AddStringToObject(metadata, "originalFolder", record->folder.c_str());
  char *encoded = cJSON_Print(metadata);
  const String metadataPath = record->directory + "/" +
      kCapsuleArchiveMetadata;
  const bool wrote = encoded != nullptr &&
      writeTextAtomic(metadataPath, String(encoded) + "\n");
  cJSON_free(encoded);
  cJSON_Delete(metadata);
  if (!wrote) return false;
  const String target = String(kCapsuleArchive) + "/" + id;
  if (fs_->exists(target) || !fs_->rename(record->directory, target)) {
    fs_->remove(metadataPath);
    return false;
  }
  return scan();
}

bool CapsuleLibrary::unarchive(const String &id) {
  const CapsuleSummary *record = find(id);
  if (record == nullptr || !record->archived || record->trashed ||
      record->status == CapsuleStatus::transcribing) return false;
  const String metadataText = readText(
      record->directory + "/" + kCapsuleArchiveMetadata, kMaxMetadataBytes);
  cJSON *metadata = cJSON_ParseWithLength(metadataText.c_str(),
                                          metadataText.length());
  const char *original = metadata == nullptr
      ? nullptr : jsonString(metadata, "originalFolder");
  String targetDirectory = safeArchiveOriginalFolder(original)
      ? safeRestoreDirectory(original) : String(kCapsuleInbox);
  cJSON_Delete(metadata);
  String target = targetDirectory + "/" + id;
  if (fs_->exists(target) && targetDirectory != kCapsuleInbox) {
    targetDirectory = kCapsuleInbox;
    target = targetDirectory + "/" + id;
  }
  if (fs_->exists(target) || !fs_->rename(record->directory, target)) {
    return false;
  }
  fs_->remove(target + "/" + kCapsuleArchiveMetadata);
  return scan();
}

bool CapsuleLibrary::trash(const String &id, const String &trashedAt) {
  const CapsuleSummary *record = find(id);
  if (record == nullptr || record->trashed || trashedAt.isEmpty() ||
      record->status == CapsuleStatus::transcribing) return false;
  cJSON *metadata = cJSON_CreateObject();
  if (metadata == nullptr) return false;
  cJSON_AddNumberToObject(metadata, "schemaVersion", 1);
  cJSON_AddStringToObject(metadata, "capsuleId", id.c_str());
  cJSON_AddStringToObject(metadata, "trashedAt", trashedAt.c_str());
  cJSON_AddStringToObject(metadata, "originalFolder", record->folder.c_str());
  cJSON_AddNumberToObject(metadata, "revision", 1);
  char *encoded = cJSON_Print(metadata);
  const bool wrote = encoded != nullptr && writeTextAtomic(
      record->directory + "/trash.json", String(encoded) + "\n");
  cJSON_free(encoded);
  cJSON_Delete(metadata);
  const String target = String(kCapsuleTrash) + "/" + id;
  if (!wrote) return false;
  if (fs_->exists(target) || !fs_->rename(record->directory, target)) {
    fs_->remove(record->directory + "/trash.json");
    return false;
  }
  return scan();
}

bool CapsuleLibrary::restore(const String &id) {
  const CapsuleSummary *record = find(id);
  if (record == nullptr || !record->trashed) return false;
  const String metadataText = readText(record->directory + "/trash.json",
                                       kMaxMetadataBytes);
  cJSON *metadata = cJSON_ParseWithLength(metadataText.c_str(),
                                          metadataText.length());
  const char *original = metadata == nullptr
      ? nullptr : jsonString(metadata, "originalFolder");
  String targetDirectory = safeRestoreDirectory(
      original == nullptr ? String() : String(original));
  cJSON_Delete(metadata);
  String target = targetDirectory + "/" + id;
  if (fs_->exists(target) && targetDirectory != kCapsuleInbox) {
    targetDirectory = kCapsuleInbox;
    target = targetDirectory + "/" + id;
  }
  if (fs_->exists(target) || !fs_->rename(record->directory, target)) {
    return false;
  }
  fs_->remove(target + "/trash.json");
  return scan();
}

CapsuleBatchResult CapsuleLibrary::batch(
    const std::vector<String> &ids, CapsuleBatchAction action,
    const String &changedAt) {
  CapsuleBatchResult result;
  if (ids.empty()) return result;
  bool favoriteTarget = true;
  if (action == CapsuleBatchAction::favorite) {
    favoriteTarget = false;
    for (const String &id : ids) {
      const CapsuleSummary *record = find(id);
      if (record != nullptr && !record->favorite) {
        favoriteTarget = true;
        break;
      }
    }
  }
  for (const String &id : ids) {
    const CapsuleSummary *record = find(id);
    if (record == nullptr || record->status == CapsuleStatus::transcribing) {
      result.failedId = id;
      return result;
    }
    if (action == CapsuleBatchAction::trashOrRestore &&
        !record->trashed && changedAt.isEmpty()) {
      result.failedId = id;
      return result;
    }
  }

  std::vector<String> changed;
  changed.reserve(ids.size());
  for (const String &id : ids) {
    const CapsuleSummary *record = find(id);
    bool ok = false;
    bool changedThisItem = true;
    if (action == CapsuleBatchAction::favorite) {
      changedThisItem = record != nullptr &&
          record->favorite != favoriteTarget;
      ok = !changedThisItem;
      if (!ok) ok = toggleFavorite(id);
    } else if (action == CapsuleBatchAction::archiveOrRestore) {
      ok = scope_ == CapsuleScope::trash ? restore(id) :
          (scope_ == CapsuleScope::archive ? unarchive(id) : archive(id));
    } else {
      ok = scope_ == CapsuleScope::trash ? restore(id)
                                         : trash(id, changedAt);
    }
    if (!ok) {
      result.failedId = id;
      CapsuleRollbackCounts rollback;
      for (auto iterator = changed.rbegin(); iterator != changed.rend();
           ++iterator) {
        bool rollbackOk = false;
        if (action == CapsuleBatchAction::favorite) {
          rollbackOk = toggleFavorite(*iterator);
        } else if (action == CapsuleBatchAction::archiveOrRestore) {
          if (scope_ == CapsuleScope::trash) {
            rollbackOk = trash(*iterator, changedAt);
          } else if (scope_ == CapsuleScope::archive) {
            rollbackOk = archive(*iterator);
          } else {
            rollbackOk = unarchive(*iterator);
          }
        } else if (scope_ == CapsuleScope::trash) {
          rollbackOk = trash(*iterator, changedAt);
        } else {
          rollbackOk = restore(*iterator);
        }
        rollback.record(rollbackOk);
        if (!rollbackOk && result.rollbackFailedId.isEmpty()) {
          result.rollbackFailedId = *iterator;
        }
      }
      result.rollbackAttempted = rollback.attempted;
      result.rollbackFailed = rollback.failed;
      result.rolledBackFully = rollback.fullyRolledBack();
      result.changed = rollback.failed;
      return result;
    }
    if (changedThisItem) changed.push_back(id);
  }
  result.ok = true;
  result.changed = changed.size();
  return result;
}

void CapsuleLibrary::setScope(CapsuleScope scope) {
  if (scope_ == scope) return;
  scope_ = scope;
  rebuildVisible();
}

void CapsuleLibrary::rebuildVisible() {
  visible_.clear();
  for (size_t index = 0; index < records_.size(); ++index) {
    const CapsuleSummary &record = records_[index];
    const bool pending = record.status == CapsuleStatus::queued ||
        record.status == CapsuleStatus::transcribing;
    if (capsuleVisibleInScope(scope_, record.favorite, pending,
                              record.status == CapsuleStatus::failed,
                              record.archived, record.trashed)) {
      visible_.push_back(index);
    }
  }
  ++revision_;
  if (revision_ == 0) revision_ = 1;
}

String CapsuleLibrary::safeRestoreDirectory(const String &folder) const {
  if (folder.isEmpty() || folder.startsWith(".") || folder.startsWith("/") ||
      folder.indexOf("\\") >= 0 || folder.indexOf("//") >= 0 ||
      folder == "." || folder == ".." || folder.indexOf("/../") >= 0 ||
      folder.startsWith("../") || folder.endsWith("/..")) {
    return String(kCapsuleInbox);
  }
  const String candidate = String(kCapsuleRoot) + "/" + folder;
  File directory = fs_->open(candidate);
  const bool valid = directory && directory.isDirectory();
  if (directory) directory.close();
  return valid ? candidate : String(kCapsuleInbox);
}

String CapsuleLibrary::readBestText(const CapsuleSummary &record,
                                    size_t maxBytes) const {
  for (const char *name : {"final.md", "polished.md", "raw.txt"}) {
    const String value = readText(record.directory + "/" + name, maxBytes);
    if (!value.isEmpty()) return value;
  }
  return record.title;
}

String CapsuleLibrary::readText(const String &path, size_t maxBytes) const {
  if (fs_ == nullptr || maxBytes == 0) return String();
  File file = fs_->open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return String();
  }
  String value;
  const size_t wanted = file.size() < maxBytes ? file.size() : maxBytes;
  if (!value.reserve(wanted + 1)) {
    file.close();
    return String();
  }
  while (file.available() && value.length() < wanted) {
    value += static_cast<char>(file.read());
  }
  file.close();
  value.trim();
  return value;
}

bool CapsuleLibrary::writeTextAtomic(const String &path, const String &value) {
  const String temporary = path + ".tmp";
  if (fs_->exists(temporary)) fs_->remove(temporary);
  File file = fs_->open(temporary, FILE_WRITE);
  if (!file) return false;
  const size_t written = file.print(value);
  const bool ok = written == value.length() && file.getWriteError() == 0;
  file.flush();
  file.close();
  if (!ok) {
    fs_->remove(temporary);
    return false;
  }
  if (fs_->exists(path) && !fs_->remove(path)) return false;
  return fs_->rename(temporary, path);
}

bool CapsuleLibrary::updateProcessing(const String &id, CapsuleStatus status,
                                      const String &rawTextFile,
                                      const String &errorStage,
                                      const String &error,
                                      bool incrementAttempts) {
  const CapsuleSummary *record = find(id);
  if (record == nullptr) return false;
  const String path = record->directory + "/processing.json";
  const String source = readText(path, kMaxMetadataBytes);
  cJSON *root = cJSON_ParseWithLength(source.c_str(), source.length());
  if (root == nullptr) return false;
  cJSON *revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
  const int nextRevision = cJSON_IsNumber(revision) ? revision->valueint + 1 : 1;
  cJSON_ReplaceItemInObjectCaseSensitive(root, "revision", cJSON_CreateNumber(nextRevision));
  cJSON_ReplaceItemInObjectCaseSensitive(root, "status",
                                         cJSON_CreateString(statusName(status)));
  if (!rawTextFile.isEmpty()) replaceStringOrNull(root, "rawTextFile", rawTextFile);
  replaceStringOrNull(root, "errorStage", errorStage);
  replaceStringOrNull(root, "error", error);
  if (incrementAttempts) {
    cJSON *attempts = cJSON_GetObjectItemCaseSensitive(root, "attempts");
    const int count = cJSON_IsNumber(attempts) ? attempts->valueint + 1 : 1;
    cJSON_ReplaceItemInObjectCaseSensitive(root, "attempts", cJSON_CreateNumber(count));
  }
  char *encoded = cJSON_Print(root);
  const bool ok = encoded != nullptr && writeTextAtomic(path, String(encoded) + "\n");
  cJSON_free(encoded);
  cJSON_Delete(root);
  return ok;
}

bool CapsuleLibrary::updateFavorite(const String &id, bool favorite) {
  const CapsuleSummary *record = find(id);
  if (record == nullptr) return false;
  const String path = record->directory + "/capsule.json";
  const String source = readText(path, kMaxMetadataBytes);
  cJSON *root = cJSON_ParseWithLength(source.c_str(), source.length());
  if (root == nullptr) return false;
  cJSON *revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
  const int nextRevision = cJSON_IsNumber(revision) ? revision->valueint + 1 : 1;
  cJSON_ReplaceItemInObjectCaseSensitive(root, "revision", cJSON_CreateNumber(nextRevision));
  cJSON_ReplaceItemInObjectCaseSensitive(root, "favorite", cJSON_CreateBool(favorite));
  char *encoded = cJSON_Print(root);
  const bool ok = encoded != nullptr && writeTextAtomic(path, String(encoded) + "\n");
  cJSON_free(encoded);
  cJSON_Delete(root);
  return ok;
}

const char *CapsuleLibrary::statusName(CapsuleStatus status) {
  switch (status) {
    case CapsuleStatus::recording: return "recording";
    case CapsuleStatus::queued: return "queued";
    case CapsuleStatus::transcribing: return "transcribing";
    case CapsuleStatus::rawReady: return "raw_ready";
    case CapsuleStatus::correcting: return "correcting";
    case CapsuleStatus::ready: return "ready";
    case CapsuleStatus::failed: return "failed";
    case CapsuleStatus::damaged: return "damaged";
  }
  return "damaged";
}

}  // namespace pokepod
