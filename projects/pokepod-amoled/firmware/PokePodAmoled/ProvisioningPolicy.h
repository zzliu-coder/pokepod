#pragma once

#include <stddef.h>
#include <stdint.h>

#include "MonotonicTime.h"

namespace pokepod {

constexpr size_t kProvisioningPasswordLength = 10;
constexpr char kProvisioningPasswordAlphabet[] =
    "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
constexpr size_t kProvisioningPasswordAlphabetLength =
    sizeof(kProvisioningPasswordAlphabet) - 1U;
static_assert(kProvisioningPasswordAlphabetLength == 32U,
              "provisioning alphabet must preserve unbiased 5-bit mapping");

using ProvisioningRandomFill = bool (*)(void *context, uint8_t *destination,
                                        size_t length);

// Production passes esp_fill_random through this narrow adapter. The bounded
// rejection loop fails closed if a source cannot provide acceptable bytes.
inline bool generateProvisioningPassword(char *destination, size_t capacity,
                                         ProvisioningRandomFill fill,
                                         void *context) {
  if (destination == nullptr || capacity <= kProvisioningPasswordLength ||
      fill == nullptr) {
    if (destination != nullptr && capacity > 0U) destination[0] = '\0';
    return false;
  }
  destination[0] = '\0';
  constexpr size_t kRandomBatchBytes = 16;
  constexpr size_t kMaximumBatches = 64;
  constexpr uint16_t kAcceptanceLimit =
      static_cast<uint16_t>((256U / kProvisioningPasswordAlphabetLength) *
                            kProvisioningPasswordAlphabetLength);
  uint8_t random[kRandomBatchBytes] = {};
  size_t written = 0;
  for (size_t batch = 0;
       written < kProvisioningPasswordLength && batch < kMaximumBatches;
       ++batch) {
    if (!fill(context, random, sizeof(random))) {
      destination[0] = '\0';
      return false;
    }
    for (const uint8_t value : random) {
      if constexpr (kAcceptanceLimit < 256U) {
        if (value >= kAcceptanceLimit) continue;
      }
      destination[written++] = kProvisioningPasswordAlphabet[
          value % kProvisioningPasswordAlphabetLength];
      if (written == kProvisioningPasswordLength) break;
    }
  }
  if (written != kProvisioningPasswordLength) {
    destination[0] = '\0';
    return false;
  }
  destination[written] = '\0';
  return true;
}

class ProvisioningCredentialPolicy {
 public:
  bool begin(ProvisioningRandomFill fill, void *context) {
    close();
    active_ = generateProvisioningPassword(password_, sizeof(password_), fill,
                                           context);
    return active_;
  }

  void close() {
    volatile char *wipe = password_;
    for (size_t index = 0; index < sizeof(password_); ++index) wipe[index] = '\0';
    active_ = false;
  }

  bool active() const { return active_; }
  const char *password() const { return active_ ? password_ : ""; }

 private:
  char password_[kProvisioningPasswordLength + 1] = {};
  bool active_ = false;
};

enum class ProvisioningSensitiveAction : uint8_t {
  none,
  replaceTencent,
  clearTencent,
};

inline const char *provisioningSensitiveActionName(
    ProvisioningSensitiveAction action) {
  switch (action) {
    case ProvisioningSensitiveAction::none: return "none";
    case ProvisioningSensitiveAction::replaceTencent: return "replaceTencent";
    case ProvisioningSensitiveAction::clearTencent: return "clearTencent";
  }
  return "none";
}

class ProvisioningSensitiveConfirmationPolicy {
 public:
  static constexpr uint32_t kConfirmationLifetimeMs = 30UL * 1000UL;

  bool begin(ProvisioningSensitiveAction action, uint32_t nowMs) {
    if (action == ProvisioningSensitiveAction::none || pending_) return false;
    action_ = action;
    requestedAtMs_ = nowMs;
    pending_ = true;
    return true;
  }

  bool acceptPhysicalPress(uint32_t nowMs) {
    if (!pending_) return false;
    if (monotonicElapsedAtLeast(nowMs, requestedAtMs_,
                                kConfirmationLifetimeMs)) {
      reset();
      return false;
    }
    reset();
    return true;
  }

  bool expire(uint32_t nowMs) {
    if (!pending_ ||
        !monotonicElapsedAtLeast(nowMs, requestedAtMs_,
                                 kConfirmationLifetimeMs)) {
      return false;
    }
    reset();
    return true;
  }

  uint32_t remainingMs(uint32_t nowMs) const {
    if (!pending_) return 0;
    const uint32_t elapsed = monotonicElapsedOrZero(nowMs, requestedAtMs_);
    if (elapsed >= kConfirmationLifetimeMs) return 0;
    return kConfirmationLifetimeMs - elapsed;
  }

  void reset() {
    pending_ = false;
    action_ = ProvisioningSensitiveAction::none;
    requestedAtMs_ = 0;
  }

  bool pending() const { return pending_; }
  ProvisioningSensitiveAction action() const { return action_; }

 private:
  bool pending_ = false;
  ProvisioningSensitiveAction action_ = ProvisioningSensitiveAction::none;
  uint32_t requestedAtMs_ = 0;
};

enum class ProvisioningState : uint8_t {
  ready,
  scanning,
  connecting,
  connected,
  error,
};

inline const char *provisioningStateName(ProvisioningState state) {
  switch (state) {
    case ProvisioningState::ready: return "ready";
    case ProvisioningState::scanning: return "scanning";
    case ProvisioningState::connecting: return "connecting";
    case ProvisioningState::connected: return "connected";
    case ProvisioningState::error: return "error";
  }
  return "ready";
}

}  // namespace pokepod
