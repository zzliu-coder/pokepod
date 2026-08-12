#pragma once

#include <stddef.h>
#include <stdint.h>

#include "FirmwarePolicy.h"
#include "WavFormat.h"

namespace pokepod {

constexpr uint64_t kMaximumRecordingAudioBytes =
    (static_cast<uint64_t>(kCapsuleByteRate) * kMaxCapsuleDurationMs) / 1000ULL;
constexpr uint64_t kMaximumRecordingWavBytes =
    kMaximumRecordingAudioBytes + kWavHeaderBytes;
constexpr uint64_t kRecordingSafetyReserveBytes = 512ULL * 1024ULL;
constexpr uint64_t kRecordingRequiredFreeBytes =
    kMaximumRecordingWavBytes + kRecordingSafetyReserveBytes;

// Arduino-ESP32 3.3.8 does not expose a cancellable File preallocation API.
// Admission therefore performs a bounded 128 KiB background probe on the
// recorder storage task before capture starts. Every write+flush primitive
// must fit inside the 2.56 s PSRAM queue safety window and aggregate throughput
// must remain at least twice the 32 kB/s PCM production rate.
constexpr uint32_t kRecordingStorageQueueSafetyMs = 128U * 20U;
constexpr size_t kRecordingProbeChunkBytes = 4U * 1024U;
constexpr uint8_t kRecordingProbeChunkCount = 32U;
constexpr uint32_t kRecordingProbeBytes =
    static_cast<uint32_t>(kRecordingProbeChunkBytes) *
    kRecordingProbeChunkCount;
constexpr uint32_t kRecordingProbeMinimumBytesPerSecond =
    kCapsuleByteRate * 2U;
constexpr uint64_t kRecordingProbeMaximumTailUs =
    static_cast<uint64_t>(kRecordingStorageQueueSafetyMs) * 1000ULL;
constexpr uint64_t kRecordingProbeMaximumTotalUs =
    (static_cast<uint64_t>(kRecordingProbeBytes) * 1000000ULL) /
    kRecordingProbeMinimumBytesPerSecond;

static_assert(kMaximumRecordingAudioBytes == 1872000ULL,
              "58.5 seconds of 16 kHz mono PCM must use 1,872,000 bytes");
static_assert(kMaximumRecordingWavBytes == 1872044ULL,
              "maximum WAV must include the 44-byte header");

struct RecordingSpaceSnapshot {
  uint64_t totalBytes = 0;
  uint64_t usedBytes = 0;
  bool known = false;
};

enum class RecordingAdmissionReason : uint8_t {
  allowed = 0,
  capacityUnknown,
  invalidCapacity,
  insufficientSpace,
};

struct RecordingAdmission {
  RecordingAdmissionReason reason = RecordingAdmissionReason::capacityUnknown;
  uint64_t availableBytes = 0;
  uint64_t requiredBytes = kRecordingRequiredFreeBytes;

  bool allowed() const { return reason == RecordingAdmissionReason::allowed; }
};

constexpr RecordingAdmission evaluateRecordingAdmission(
    const RecordingSpaceSnapshot &space) {
  if (!space.known) {
    return {RecordingAdmissionReason::capacityUnknown, 0,
            kRecordingRequiredFreeBytes};
  }
  if (space.usedBytes > space.totalBytes) {
    return {RecordingAdmissionReason::invalidCapacity, 0,
            kRecordingRequiredFreeBytes};
  }
  const uint64_t available = space.totalBytes - space.usedBytes;
  return {available >= kRecordingRequiredFreeBytes
              ? RecordingAdmissionReason::allowed
              : RecordingAdmissionReason::insufficientSpace,
          available, kRecordingRequiredFreeBytes};
}

}  // namespace pokepod
