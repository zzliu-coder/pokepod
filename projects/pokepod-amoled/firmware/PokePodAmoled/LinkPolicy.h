#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pokepod {

constexpr size_t kLinkRecentRequestCount = 32;

enum class PurgeOutcome : uint8_t {
  rejected,
  committed,
  committedCleanupDeferred,
};

inline PurgeOutcome purgeOutcome(bool staged, bool removed) {
  if (!staged) return PurgeOutcome::rejected;
  return removed ? PurgeOutcome::committed
                 : PurgeOutcome::committedCleanupDeferred;
}

inline bool purgeStagingDirectoryName(const char *value) {
  return value != nullptr && strncmp(value, "purge-", 6) == 0 &&
      value[6] != '\0';
}

constexpr bool linkStorageBusy(bool recording, bool transcribing) {
  return recording || transcribing;
}

constexpr bool linkForegroundBusy(bool recording, bool transcribing,
                                  bool bleVoiceActive,
                                  bool captureAvailable) {
  return linkStorageBusy(recording, transcribing) || bleVoiceActive ||
      !captureAvailable;
}

constexpr bool transcriptionDispatchBusy(bool recording,
                                         bool maintenanceActive) {
  return recording || maintenanceActive;
}

inline bool safeLinkRelativePath(const char *value, bool allowHiddenRoot = false) {
  if (value == nullptr || value[0] == '\0' || value[0] == '/' ||
      strlen(value) > 240) {
    return false;
  }
  size_t segmentLength = 0;
  size_t segmentIndex = 0;
  for (size_t index = 0;; ++index) {
    const char c = value[index];
    if (c == '\\' || (c != '\0' && static_cast<unsigned char>(c) < 0x20)) {
      return false;
    }
    if (c == '/' || c == '\0') {
      if (segmentLength == 0) return false;
      const size_t start = index - segmentLength;
      if ((segmentLength == 1 && value[start] == '.') ||
          (segmentLength == 2 && value[start] == '.' && value[start + 1] == '.')) {
        return false;
      }
      if (segmentIndex == 0 && value[start] == '.' && !allowHiddenRoot) return false;
      ++segmentIndex;
      segmentLength = 0;
      if (c == '\0') break;
    } else {
      ++segmentLength;
    }
  }
  return true;
}

class LinkRequestHistory {
 public:
  bool contains(uint32_t requestId) const {
    if (requestId == 0) return true;
    for (size_t index = 0; index < count_; ++index) {
      if (values_[index] == requestId) return true;
    }
    return false;
  }

  bool complete(uint32_t requestId) {
    if (contains(requestId)) return false;
    values_[next_] = requestId;
    next_ = (next_ + 1) % kLinkRecentRequestCount;
    if (count_ < kLinkRecentRequestCount) ++count_;
    return true;
  }

  void clear() {
    count_ = 0;
    next_ = 0;
  }

 private:
  uint32_t values_[kLinkRecentRequestCount] = {};
  size_t count_ = 0;
  size_t next_ = 0;
};

}  // namespace pokepod
