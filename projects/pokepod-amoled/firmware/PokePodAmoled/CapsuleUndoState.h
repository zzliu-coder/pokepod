#pragma once

#include <algorithm>
#include <stdint.h>
#include <string>
#include <vector>

namespace pokepod {

struct CapsuleUndoResult {
  size_t attempted = 0;
  size_t restored = 0;
  std::vector<std::string> failedIds;

  size_t failed() const { return failedIds.size(); }
  bool complete() const { return attempted > 0 && failedIds.empty(); }
};

class CapsuleUndoState {
 public:
  static constexpr uint32_t kDurationMs = 5000;

  void arm(const std::vector<std::string> &ids, uint32_t nowMs) {
    pendingIds_.clear();
    for (const std::string &id : ids) {
      if (id.empty() ||
          std::find(pendingIds_.begin(), pendingIds_.end(), id) !=
              pendingIds_.end()) {
        continue;
      }
      pendingIds_.push_back(id);
    }
    deadlineMs_ = pendingIds_.empty() ? 0 : nowMs + kDurationMs;
  }

  bool available(uint32_t nowMs) const {
    return !pendingIds_.empty() &&
        static_cast<int32_t>(deadlineMs_ - nowMs) > 0;
  }

  uint32_t remainingMs(uint32_t nowMs) const {
    return available(nowMs) ? deadlineMs_ - nowMs : 0;
  }

  const std::vector<std::string> &pendingIds() const { return pendingIds_; }

  CapsuleUndoResult finishAttempt(
      const std::vector<std::string> &failedIds) {
    CapsuleUndoResult result;
    result.attempted = pendingIds_.size();
    for (const std::string &pending : pendingIds_) {
      if (std::find(failedIds.begin(), failedIds.end(), pending) !=
          failedIds.end()) {
        result.failedIds.push_back(pending);
      }
    }
    result.restored = result.attempted - result.failedIds.size();
    pendingIds_ = result.failedIds;
    if (pendingIds_.empty()) deadlineMs_ = 0;
    return result;
  }

  bool expire(uint32_t nowMs) {
    if (pendingIds_.empty() || available(nowMs)) return false;
    clear();
    return true;
  }

  void clear() {
    pendingIds_.clear();
    deadlineMs_ = 0;
  }

 private:
  std::vector<std::string> pendingIds_;
  uint32_t deadlineMs_ = 0;
};

}  // namespace pokepod
