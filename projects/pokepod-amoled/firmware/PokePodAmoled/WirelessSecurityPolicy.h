#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <utility>

#include "SecureWipe.h"

namespace pokepod {

constexpr uint32_t kWirelessAuthenticationTimeoutMs = 6000;
constexpr uint32_t kPairingExportRetryWindowSeconds = 120;

inline bool securityTimeReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

inline bool securityTokenEqual(const std::string &left,
                               const std::string &right) {
  if (left.size() != right.size()) return false;
  uint8_t difference = 0;
  for (size_t index = 0; index < left.size(); ++index) {
    difference |= static_cast<uint8_t>(left[index]) ^
                  static_cast<uint8_t>(right[index]);
  }
  return difference == 0;
}

class ProvisioningCsrfPolicy {
 public:
  void begin(std::string token, uint32_t nowMs, uint32_t lifetimeMs) {
    close();
    token_ = std::move(token);
    deadlineMs_ = nowMs + lifetimeMs;
    active_ = !token_.empty() && lifetimeMs != 0;
    secureWipe(token);
  }

  void close() {
    active_ = false;
    deadlineMs_ = 0;
    secureWipe(token_);
  }

  bool accepts(const std::string &candidate, uint32_t nowMs) const {
    return active_ && !securityTimeReached(nowMs, deadlineMs_) &&
           securityTokenEqual(candidate, token_);
  }

  const std::string &token() const { return token_; }

 private:
  std::string token_;
  uint32_t deadlineMs_ = 0;
  bool active_ = false;
};

class WirelessAuthDeadline {
 public:
  void reset() {
    armed_ = false;
    startedAtMs_ = 0;
  }

  void observeTlsReady(uint32_t nowMs) {
    if (!armed_) {
      armed_ = true;
      startedAtMs_ = nowMs;
    }
  }

  bool expired(uint32_t nowMs) const {
    return armed_ && static_cast<uint32_t>(nowMs - startedAtMs_) >=
                         kWirelessAuthenticationTimeoutMs;
  }

  bool armed() const { return armed_; }
  uint32_t startedAtMs() const { return startedAtMs_; }

 private:
  bool armed_ = false;
  uint32_t startedAtMs_ = 0;
};

class PairingExportRotationPolicy {
 public:
  bool shouldRotate(bool requested, uint32_t nowSeconds) {
    if (!requested) return false;
    if (pending_ && !securityTimeReached(nowSeconds, reuseUntilSeconds_)) {
      // A retry returns a freshly dated bundle with the same secret. Keep that
      // bundle valid for its complete advertised lifetime as well.
      reuseUntilSeconds_ = nowSeconds + kPairingExportRetryWindowSeconds;
      return false;
    }
    pending_ = true;
    reuseUntilSeconds_ = nowSeconds + kPairingExportRetryWindowSeconds;
    return true;
  }

  void reset() {
    pending_ = false;
    reuseUntilSeconds_ = 0;
  }

  void rotationFailed() { reset(); }

 private:
  bool pending_ = false;
  uint32_t reuseUntilSeconds_ = 0;
};

}  // namespace pokepod
