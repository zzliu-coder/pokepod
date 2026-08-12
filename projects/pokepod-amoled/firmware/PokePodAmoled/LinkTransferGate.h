#pragma once

#include <stdint.h>

namespace pokepod {

class LinkTransferCancellationSink {
 public:
  virtual ~LinkTransferCancellationSink() = default;
  virtual void cancelForTransferDeadline() = 0;
};

class LinkTransferGate {
 public:
  virtual ~LinkTransferGate() = default;
  virtual bool permits(uint32_t nowMs) = 0;
  virtual void attachCancellationSink(LinkTransferCancellationSink *sink) = 0;
  virtual void detachCancellationSink(LinkTransferCancellationSink *sink) = 0;
  // Exposes an already-armed absolute deadline to request lifecycle owners.
  // Reading this value never arms, extends or otherwise mutates the gate.
  virtual bool absoluteDeadline(uint32_t &deadlineMs) const {
    (void)deadlineMs;
    return false;
  }
};

inline bool linkTransferPermitted(LinkTransferGate *gate,
                                  uint32_t nowMs) {
  return gate == nullptr || gate->permits(nowMs);
}

class AbsoluteLinkDeadlineGate final : public LinkTransferGate {
 public:
  void arm(uint32_t nowMs, uint32_t durationMs) {
    deadlineMs_ = nowMs + durationMs;
    active_ = true;
    cancellationSent_ = false;
  }

  void cancel() {
    active_ = false;
    deadlineMs_ = 0;
    cancellationSent_ = false;
  }

  bool permits(uint32_t nowMs) override {
    const bool allowed = active_ &&
        static_cast<int32_t>(nowMs - deadlineMs_) < 0;
    if (!allowed && active_ && !cancellationSent_ && sink_ != nullptr) {
      cancellationSent_ = true;
      LinkTransferCancellationSink *sink = sink_;
      sink->cancelForTransferDeadline();
    }
    return allowed;
  }

  bool expired(uint32_t nowMs) const {
    return active_ && static_cast<int32_t>(nowMs - deadlineMs_) >= 0;
  }

  void attachCancellationSink(LinkTransferCancellationSink *sink) override {
    sink_ = sink;
  }

  void detachCancellationSink(LinkTransferCancellationSink *sink) override {
    if (sink_ == sink) sink_ = nullptr;
  }

  bool absoluteDeadline(uint32_t &deadlineMs) const override {
    if (!active_) return false;
    deadlineMs = deadlineMs_;
    return true;
  }

  bool active() const { return active_; }
  uint32_t deadlineMs() const { return deadlineMs_; }

 private:
  bool active_ = false;
  bool cancellationSent_ = false;
  uint32_t deadlineMs_ = 0;
  LinkTransferCancellationSink *sink_ = nullptr;
};

}  // namespace pokepod
