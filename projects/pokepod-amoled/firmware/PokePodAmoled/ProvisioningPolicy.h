#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

constexpr char kProvisioningPassword[] = "88888888";
constexpr size_t kProvisioningPasswordLength = sizeof(kProvisioningPassword) - 1;

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
