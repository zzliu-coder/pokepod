#pragma once

#include <stdint.h>

#include "TencentAsrControl.h"
#include "TencentJobModel.h"

namespace pokepod {

enum class TencentQuiesceStatus : uint8_t {
  complete = 0,
  waiting,
  timedOut,
};

// Arduino-free production runtime.  TencentWorker delegates every lifecycle
// transition to this object; host tests exercise the exact same arbitration
// used around the network request and storage commit.
class TencentJobRuntime {
 public:
  void workerStartResult(bool created) {
    if (created) {
      model_.resetIdle();
    } else {
      model_.failWithoutActiveJob();
    }
  }

  uint32_t request(uint32_t deadlineMs) {
    if (tencentJobOwnsResources(model_.state())) return 0;
    const uint32_t generation = cancelToken_.begin();
    if (!model_.queue(generation, deadlineMs)) return 0;
    quiesceActive_ = false;
    return generation;
  }

  bool start(uint32_t generation) {
    return !cancelToken_.cancelled(generation) && model_.start(generation);
  }

  bool cancel(TencentCancelReason reason) {
    const uint32_t generation = model_.generation();
    if (!model_.cancel(generation, reason)) return false;
    cancelToken_.cancel(generation);
    return true;
  }

  bool checkWatchdog(uint32_t nowMs) {
    if (!model_.checkWatchdog(nowMs)) return false;
    cancelToken_.cancel(model_.generation());
    return true;
  }

  bool networkFinished(uint32_t generation, bool ok, bool transient) {
    return model_.networkFinished(generation, ok, transient);
  }

  bool commitFinished(uint32_t generation, bool committed) {
    return model_.commitFinished(generation, committed);
  }

  void failBeforeRequest() { model_.failWithoutActiveJob(); }

  TencentQuiesceStatus beginQuiesce(uint32_t nowMs, uint32_t timeoutMs,
                                    TencentCancelReason reason) {
    if (!working()) {
      quiesceActive_ = false;
      return TencentQuiesceStatus::complete;
    }
    cancel(reason);
    quiesceDeadlineMs_ = nowMs + timeoutMs;
    quiesceActive_ = true;
    return pollQuiesce(nowMs);
  }

  TencentQuiesceStatus pollQuiesce(uint32_t nowMs) {
    if (!working()) {
      quiesceActive_ = false;
      return TencentQuiesceStatus::complete;
    }
    if (quiesceActive_ &&
        tencentDeadlineReached(nowMs, quiesceDeadlineMs_)) {
      return TencentQuiesceStatus::timedOut;
    }
    return TencentQuiesceStatus::waiting;
  }

  TencentJobState state() const { return model_.state(); }
  TencentCancelReason cancelReason() const { return model_.cancelReason(); }
  uint32_t generation() const { return model_.generation(); }
  uint32_t deadlineMs() const { return model_.deadlineMs(); }
  bool matchesGeneration(uint32_t generation) const {
    return model_.matchesGeneration(generation);
  }
  bool working() const { return tencentJobOwnsResources(state()); }
  TencentCancelToken *cancelToken() { return &cancelToken_; }

 private:
  TencentJobModel model_;
  TencentCancelToken cancelToken_;
  bool quiesceActive_ = false;
  uint32_t quiesceDeadlineMs_ = 0;
};

}  // namespace pokepod
