#pragma once

#include <stdint.h>

namespace pokepod {

enum class DeviceCapability : uint16_t {
  display = 1U << 0,
  touch = 1U << 1,
  storage = 1U << 2,
  recording = 1U << 3,
  bleVoice = 1U << 4,
  link = 1U << 5,
  wifi = 1U << 6,
  rtc = 1U << 7,
  imu = 1U << 8,
  pmu = 1U << 9,
  audio = 1U << 10,
};

constexpr uint16_t capabilityMask(DeviceCapability capability) {
  return static_cast<uint16_t>(capability);
}

constexpr uint16_t capabilityMask(DeviceCapability first,
                                  DeviceCapability second) {
  return capabilityMask(first) | capabilityMask(second);
}

constexpr uint16_t kAllDeviceCapabilities =
    capabilityMask(DeviceCapability::display) |
    capabilityMask(DeviceCapability::touch) |
    capabilityMask(DeviceCapability::storage) |
    capabilityMask(DeviceCapability::recording) |
    capabilityMask(DeviceCapability::bleVoice) |
    capabilityMask(DeviceCapability::link) |
    capabilityMask(DeviceCapability::wifi) |
    capabilityMask(DeviceCapability::rtc) |
    capabilityMask(DeviceCapability::imu) |
    capabilityMask(DeviceCapability::pmu) |
    capabilityMask(DeviceCapability::audio);

class CapabilityRegistry {
 public:
  void record(DeviceCapability capability, bool ready) {
    const uint16_t bit = capabilityMask(capability);
    observed_ |= bit;
    if (ready) ready_ |= bit;
    else ready_ &= static_cast<uint16_t>(~bit);
  }

  bool observed(DeviceCapability capability) const {
    return (observed_ & capabilityMask(capability)) != 0;
  }

  bool ready(DeviceCapability capability) const {
    const uint16_t bit = capabilityMask(capability);
    return (observed_ & bit) != 0 && (ready_ & bit) != 0;
  }

  bool allows(uint16_t required) const {
    return (observed_ & required) == required &&
        (ready_ & required) == required;
  }

  bool allReady() const { return allows(kAllDeviceCapabilities); }
  uint16_t observedMask() const { return observed_; }
  uint16_t readyMask() const { return ready_; }
  uint16_t missingMask() const {
    return static_cast<uint16_t>(kAllDeviceCapabilities & ~ready_);
  }

 private:
  uint16_t observed_ = 0;
  uint16_t ready_ = 0;
};

constexpr uint16_t kRecordingCapabilities =
    capabilityMask(DeviceCapability::storage) |
    capabilityMask(DeviceCapability::audio) |
    capabilityMask(DeviceCapability::recording);
constexpr uint16_t kPlaybackCapabilities =
    capabilityMask(DeviceCapability::storage) |
    capabilityMask(DeviceCapability::audio);
constexpr uint16_t kBleVoiceCapabilities =
    capabilityMask(DeviceCapability::audio) |
    capabilityMask(DeviceCapability::bleVoice);
constexpr uint16_t kWifiCapabilities =
    capabilityMask(DeviceCapability::wifi);
constexpr uint16_t kComputerSyncCapabilities =
    capabilityMask(DeviceCapability::wifi) |
    capabilityMask(DeviceCapability::link);

}  // namespace pokepod
