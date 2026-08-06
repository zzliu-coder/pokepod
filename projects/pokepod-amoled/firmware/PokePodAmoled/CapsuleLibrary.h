#pragma once

#include <Arduino.h>
#include <FS.h>
#include <vector>

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
  String preview;
  String audioFile;
  CapsuleStatus status = CapsuleStatus::damaged;
  bool favorite = false;
  uint32_t durationMs = 0;
};

class CapsuleLibrary {
 public:
  bool begin(fs::FS &fs, Print &log);
  bool scan();
  size_t count() const { return records_.size(); }
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

  fs::FS *fs_ = nullptr;
  Print *log_ = nullptr;
  std::vector<CapsuleSummary> records_;
};

}  // namespace pokepod
