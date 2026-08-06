#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

constexpr int kDisplayWidth = 368;
constexpr int kDisplayHeight = 448;

constexpr uint32_t kAudioSampleRate = 48000;
// ES8311 exposes one physical microphone. I2S carries the same sample in two
// slots for the SD recorder, while USB advertises the truthful mono topology.
constexpr uint16_t kAudioChannels = 2;
constexpr uint16_t kUsbAudioChannels = 1;
constexpr uint16_t kAudioBitsPerSample = 16;
constexpr uint32_t kCapsuleSampleRate = 16000;
constexpr uint16_t kCapsuleChannels = 1;
constexpr uint16_t kCapsuleBitsPerSample = 16;
constexpr size_t kCapsuleBytesPerSample = kCapsuleBitsPerSample / 8;
constexpr size_t kCapsuleByteRate =
    kCapsuleSampleRate * kCapsuleChannels * kCapsuleBytesPerSample;
constexpr uint32_t kMaxCapsuleDurationMs = 58500;
constexpr uint32_t kUsbAudioIntervalsPerSecond = 1000;
constexpr size_t kAudioBytesPerSample = kAudioBitsPerSample / 8;
constexpr size_t kAudioByteRate =
    kAudioSampleRate * kAudioChannels * kAudioBytesPerSample;
constexpr size_t kAudioBytesPerChunk =
    kAudioByteRate / kUsbAudioIntervalsPerSecond;
constexpr size_t kUsbAudioBytesPerChunk =
    kAudioSampleRate * kUsbAudioChannels * kAudioBytesPerSample /
    kUsbAudioIntervalsPerSecond;

// TinyUSB's full-speed UAC endpoint formula reserves one extra sample frame
// for clock tolerance. The producer still writes exactly one nominal 1 ms
// PCM interval at a time.
constexpr size_t kUsbAudioEndpointMaxPacketBytes =
    (((kAudioSampleRate + 999) / 1000) + 1) *
    kUsbAudioChannels * kAudioBytesPerSample;

static_assert(kAudioBitsPerSample % 8 == 0,
              "PCM bits per sample must be byte aligned");
static_assert(kAudioSampleRate % kUsbAudioIntervalsPerSecond == 0,
              "USB audio packet cadence requires an integral frame count");
static_assert(kAudioBytesPerChunk == 192,
              "48 kHz stereo 16-bit I2S chunks must contain 192 bytes");
static_assert(kUsbAudioBytesPerChunk == 96,
              "48 kHz mono 16-bit UAC packets must contain 96 bytes");
static_assert(kUsbAudioBytesPerChunk <= kUsbAudioEndpointMaxPacketBytes,
              "USB audio writes must fit the full-speed endpoint packet");
static_assert(kUsbAudioEndpointMaxPacketBytes <= 1023,
              "Full-speed isochronous endpoints are limited to 1023 bytes");

inline size_t pcm16StereoLeftToMono(const uint8_t *stereo, size_t stereoBytes,
                                    uint8_t *mono, size_t monoCapacity) {
  if (stereo == nullptr || mono == nullptr) return 0;
  const size_t frames = stereoBytes / 4 < monoCapacity / 2
      ? stereoBytes / 4 : monoCapacity / 2;
  for (size_t frame = 0; frame < frames; ++frame) {
    mono[frame * 2] = stereo[frame * 4];
    mono[frame * 2 + 1] = stereo[frame * 4 + 1];
  }
  return frames * 2;
}

constexpr uint16_t pcm16PeakLittleEndian(const uint8_t *data, size_t bytes) {
  if (data == nullptr) return 0;
  uint16_t peak = 0;
  for (size_t offset = 0; offset + 1 < bytes; offset += 2) {
    const uint16_t raw = static_cast<uint16_t>(data[offset]) |
                         (static_cast<uint16_t>(data[offset + 1]) << 8);
    const int32_t sample = raw <= 0x7fff
        ? static_cast<int32_t>(raw)
        : static_cast<int32_t>(raw) - 0x10000;
    const uint16_t magnitude = static_cast<uint16_t>(
        sample < 0 ? -sample : sample);
    if (magnitude > peak) peak = magnitude;
  }
  return peak;
}

enum class BoardVariant {
  unknown,
  v1Sh8601Ft3168,
  v2Co5300Cst820,
};

constexpr BoardVariant boardVariantFromTouchProbes(bool v1, bool v2) {
  if (v1 && !v2) return BoardVariant::v1Sh8601Ft3168;
  if (v2 && !v1) return BoardVariant::v2Co5300Cst820;
  return BoardVariant::unknown;
}

inline const char *variantName(BoardVariant variant) {
  switch (variant) {
    case BoardVariant::v1Sh8601Ft3168: return "V1 SH8601/FT3168";
    case BoardVariant::v2Co5300Cst820: return "V2 CO5300/CST820";
    default: return "UNKNOWN";
  }
}

enum class TouchAction {
  none,
  dictation,
  recording,
};

constexpr TouchAction touchActionAt(int16_t x, int16_t y) {
  if (x < 16 || x >= kDisplayWidth - 16) return TouchAction::none;
  if (y >= 246 && y < 328) return TouchAction::dictation;
  if (y >= 342 && y < 424) return TouchAction::recording;
  return TouchAction::none;
}

constexpr bool deadlinePending(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(deadline - now) > 0;
}

}  // namespace pokepod
