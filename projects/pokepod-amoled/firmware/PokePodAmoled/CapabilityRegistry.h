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
  capsuleLibrary = 1U << 11,
  transcription = 1U << 12,
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
    capabilityMask(DeviceCapability::audio) |
    capabilityMask(DeviceCapability::capsuleLibrary) |
    capabilityMask(DeviceCapability::transcription);

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

constexpr uint16_t kCapsuleBrowsingCapabilities =
    capabilityMask(DeviceCapability::storage) |
    capabilityMask(DeviceCapability::capsuleLibrary);
constexpr uint16_t kRecordingCapabilities =
    kCapsuleBrowsingCapabilities |
    capabilityMask(DeviceCapability::audio) |
    capabilityMask(DeviceCapability::recording);
constexpr uint16_t kPlaybackCapabilities =
    kCapsuleBrowsingCapabilities |
    capabilityMask(DeviceCapability::audio);
constexpr uint16_t kTranscriptionCapabilities =
    kCapsuleBrowsingCapabilities |
    capabilityMask(DeviceCapability::wifi) |
    capabilityMask(DeviceCapability::transcription);
constexpr uint16_t kBleVoiceCapabilities =
    capabilityMask(DeviceCapability::audio) |
    capabilityMask(DeviceCapability::bleVoice);
constexpr uint16_t kWifiCapabilities =
    capabilityMask(DeviceCapability::wifi);
constexpr uint16_t kComputerSyncCapabilities =
    kCapsuleBrowsingCapabilities |
    capabilityMask(DeviceCapability::wifi) |
    capabilityMask(DeviceCapability::link);

enum class StartupCapabilityMode : uint8_t {
  ready,
  recordingWithoutTranscription,
  browsingOnly,
  wirelessVoiceOnly,
  degraded,
  unavailable,
};

struct StartupCapabilityPresentation {
  StartupCapabilityMode mode = StartupCapabilityMode::unavailable;
  const char *message = "关键服务启动失败";
  bool capsuleBrowsing = false;
  bool capsuleRecording = false;
  bool transcription = false;
  bool wirelessVoice = false;
  bool computerSync = false;
};

inline StartupCapabilityPresentation startupCapabilityPresentation(
    const CapabilityRegistry &capabilities) {
  StartupCapabilityPresentation result;
  result.capsuleBrowsing = capabilities.allows(kCapsuleBrowsingCapabilities);
  result.capsuleRecording = capabilities.allows(kRecordingCapabilities);
  result.transcription = capabilities.allows(kTranscriptionCapabilities);
  result.wirelessVoice = capabilities.allows(kBleVoiceCapabilities);
  result.computerSync = capabilities.allows(kComputerSyncCapabilities);

  if (capabilities.allReady()) {
    result.mode = StartupCapabilityMode::ready;
    result.message = "PokePod 已就绪";
  } else if (!result.capsuleBrowsing && result.wirelessVoice) {
    result.mode = StartupCapabilityMode::wirelessVoiceOnly;
    result.message = "本地胶囊不可用 · 只能无线语音";
  } else if (result.capsuleBrowsing && !result.capsuleRecording) {
    result.mode = StartupCapabilityMode::browsingOnly;
    result.message = "录音不可用 · 胶囊浏览可用";
  } else if (result.capsuleRecording && !result.transcription) {
    result.mode = StartupCapabilityMode::recordingWithoutTranscription;
    result.message = "胶囊可录制 · 转写服务未就绪";
  } else if (result.capsuleBrowsing || result.wirelessVoice ||
             result.computerSync) {
    result.mode = StartupCapabilityMode::degraded;
    result.message = "部分功能未就绪";
  }
  return result;
}

inline const char *startupCapabilityModeName(StartupCapabilityMode mode) {
  switch (mode) {
    case StartupCapabilityMode::ready: return "ready";
    case StartupCapabilityMode::recordingWithoutTranscription:
      return "recording-without-transcription";
    case StartupCapabilityMode::browsingOnly: return "browsing-only";
    case StartupCapabilityMode::wirelessVoiceOnly: return "wireless-voice-only";
    case StartupCapabilityMode::degraded: return "degraded";
    case StartupCapabilityMode::unavailable: return "unavailable";
  }
  return "unavailable";
}

}  // namespace pokepod
