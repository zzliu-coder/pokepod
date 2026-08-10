#pragma once

#include <Arduino.h>
#include <FS.h>
#include <vector>

#include "CapsuleBrowserState.h"

namespace pokepod {

enum class CapsuleStatus {
  recording,
  queued,
  transcribing,
  rawReady,
  correcting,
  ready,
  failed,
  damaged,
};

struct CapsuleSummary {
  String id;
  String directory;
  String folder;
  String title;
  String createdAt;
  String updatedAt;
  String preview;
  String audioFile;
  String audioFormat;
  String errorStage;
  String error;
  CapsuleStatus status = CapsuleStatus::damaged;
  bool favorite = false;
  bool archived = false;
  bool trashed = false;
  bool readOnly = false;
  int capsuleSchemaVersion = -1;
  int processingSchemaVersion = -1;
  int revision = -1;
  int processingRevision = -1;
  uint32_t durationMs = 0;
  uint32_t sampleRateHz = 0;
  uint8_t channels = 0;
  uint8_t bitsPerSample = 0;
};

enum class CapsuleBatchAction : uint8_t {
  favorite,
  archiveOrRestore,
  trashOrRestore,
};

struct CapsuleBatchResult {
  bool ok = false;
  size_t changed = 0;
  String failedId;
  size_t rollbackAttempted = 0;
  size_t rollbackFailed = 0;
  String rollbackFailedId;
  bool rolledBackFully = false;
};

class CapsuleLibrary {
 public:
  bool begin(fs::FS &fs, Print &log);
  bool scan();
  size_t count() const { return visible_.size(); }
  uint32_t revision() const { return revision_; }
  size_t pendingCount() const;
  const CapsuleSummary *at(size_t index) const;
  const CapsuleSummary *nextQueued() const;
  const CapsuleSummary *find(const String &id) const;

  bool markTranscribing(const String &id);
  bool commitRawText(const String &id, const String &text);
  bool markFailure(const String &id, const String &stage, const String &error);
  bool markRetryable(const String &id, const String &stage, const String &error);
  bool requeue(const String &id);
  bool toggleFavorite(const String &id);
  bool archive(const String &id);
  bool unarchive(const String &id);
  bool trash(const String &id, const String &trashedAt);
  bool restore(const String &id);
  CapsuleBatchResult purge(const std::vector<String> &ids);
  CapsuleBatchResult batch(const std::vector<String> &ids,
                           CapsuleBatchAction action,
                           const String &changedAt);
  void setScope(CapsuleScope scope);
  CapsuleScope scope() const { return scope_; }
  String readBestText(const CapsuleSummary &record, size_t maxBytes = 16384) const;

  static const char *statusName(CapsuleStatus status);

 private:
  void scanFolder(const String &path, const String &folder, uint8_t depth);
  bool readRecord(const String &directory, const String &folder,
                  CapsuleSummary &record) const;
  String readText(const String &path, size_t maxBytes) const;
  bool writeTextAtomic(const String &path, const String &value);
  bool updateProcessing(const String &id, CapsuleStatus status,
                        const String &rawTextFile, const String &errorStage,
                        const String &error, bool incrementAttempts);
  bool updateFavorite(const String &id, bool favorite);
  bool removeTree(const String &path);
  void rebuildVisible();
  String safeRestoreDirectory(const String &folder) const;

  fs::FS *fs_ = nullptr;
  Print *log_ = nullptr;
  std::vector<CapsuleSummary> records_;
  std::vector<size_t> visible_;
  CapsuleScope scope_ = CapsuleScope::inbox;
  uint32_t revision_ = 0;
};

}  // namespace pokepod
