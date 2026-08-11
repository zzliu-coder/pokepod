#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "RecorderOutcome.h"
#include "RecordingAdmissionPolicy.h"

namespace pokepod {

constexpr uint32_t kRecorderCheckpointMagic = 0x31435052U;  // RPC1.
constexpr uint16_t kRecorderCheckpointVersion = 1;
constexpr uint32_t kRecorderCheckpointIntervalBytes =
    static_cast<uint32_t>(kCapsuleByteRate * 2U);

enum class RecorderCheckpointState : uint8_t {
  recording = 1,
  failed = 2,
};

#pragma pack(push, 1)
struct StoredRecorderCheckpoint {
  uint32_t magic;
  uint16_t version;
  uint8_t state;
  uint8_t failureStage;
  uint32_t confirmedDataBytes;
  uint32_t audioCrc32;
  uint32_t sampleRateHz;
  uint16_t channels;
  uint16_t bitsPerSample;
  char capsuleId[37];
  char createdAt[40];
  uint32_t crc32;
};
#pragma pack(pop)

static_assert(sizeof(StoredRecorderCheckpoint) <= 112,
              "recorder checkpoint must remain a small fixed record");

inline uint32_t recorderCheckpointCrc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U &
          static_cast<uint32_t>(-static_cast<int32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

inline uint32_t recorderAudioCrc32Update(uint32_t state,
                                         const uint8_t *data,
                                         size_t length) {
  if (data == nullptr && length != 0) return state;
  for (size_t index = 0; index < length; ++index) {
    state ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      state = (state >> 1U) ^ (0xedb88320U &
          static_cast<uint32_t>(-static_cast<int32_t>(state & 1U)));
    }
  }
  return state;
}

constexpr uint32_t recorderAudioCrc32Begin() { return 0xffffffffU; }
constexpr uint32_t recorderAudioCrc32Finish(uint32_t state) { return ~state; }

inline bool recorderCheckpointCopy(char *target, size_t capacity,
                                   const char *source) {
  if (target == nullptr || capacity == 0 || source == nullptr) return false;
  size_t length = 0;
  while (length < capacity && source[length] != '\0') ++length;
  if (length == 0 || length >= capacity) return false;
  memcpy(target, source, length);
  target[length] = '\0';
  return true;
}

inline bool recorderCheckpointHasTerminator(const char *value,
                                            size_t capacity) {
  for (size_t index = 0; index < capacity; ++index) {
    if (value[index] == '\0') return true;
  }
  return false;
}

inline bool recorderCheckpointUuidShape(const char *value) {
  if (value == nullptr) return false;
  for (size_t index = 0; index < 36; ++index) {
    const char character = value[index];
    const bool hyphen = index == 8 || index == 13 || index == 18 || index == 23;
    if (hyphen) {
      if (character != '-') return false;
    } else if (!((character >= '0' && character <= '9') ||
                 (character >= 'a' && character <= 'f') ||
                 (character >= 'A' && character <= 'F'))) {
      return false;
    }
  }
  return value[36] == '\0';
}

inline bool recorderCheckpointDigits(const char *value, size_t offset,
                                     size_t count) {
  for (size_t index = 0; index < count; ++index) {
    const char character = value[offset + index];
    if (character < '0' || character > '9') return false;
  }
  return true;
}

inline uint8_t recorderCheckpointTwoDigits(const char *value,
                                           size_t offset) {
  return static_cast<uint8_t>((value[offset] - '0') * 10 +
                              (value[offset + 1] - '0'));
}

inline bool recorderCheckpointLeapYear(uint16_t year) {
  return (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
}

inline bool recorderCheckpointCreatedAtShape(const char *value) {
  if (value == nullptr ||
      !recorderCheckpointDigits(value, 0, 4) ||
      !recorderCheckpointDigits(value, 5, 2) ||
      !recorderCheckpointDigits(value, 8, 2) ||
      !recorderCheckpointDigits(value, 11, 2) ||
      !recorderCheckpointDigits(value, 14, 2) ||
      !recorderCheckpointDigits(value, 17, 2) ||
      value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':') {
    return false;
  }

  const uint16_t year = static_cast<uint16_t>(
      (value[0] - '0') * 1000 + (value[1] - '0') * 100 +
      (value[2] - '0') * 10 + (value[3] - '0'));
  const uint8_t month = recorderCheckpointTwoDigits(value, 5);
  const uint8_t day = recorderCheckpointTwoDigits(value, 8);
  const uint8_t hour = recorderCheckpointTwoDigits(value, 11);
  const uint8_t minute = recorderCheckpointTwoDigits(value, 14);
  const uint8_t second = recorderCheckpointTwoDigits(value, 17);
  if (year < 2000U || month < 1U || month > 12U || hour > 23U ||
      minute > 59U || second > 59U) {
    return false;
  }
  static constexpr uint8_t kDaysPerMonth[] = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  uint8_t maximumDay = kDaysPerMonth[month - 1U];
  if (month == 2U && recorderCheckpointLeapYear(year)) maximumDay = 29U;
  if (day < 1U || day > maximumDay) return false;

  if (value[19] == 'Z') return value[20] == '\0';
  if ((value[19] != '+' && value[19] != '-') ||
      !recorderCheckpointDigits(value, 20, 2) || value[22] != ':' ||
      !recorderCheckpointDigits(value, 23, 2) || value[25] != '\0') {
    return false;
  }
  const uint8_t offsetHour = recorderCheckpointTwoDigits(value, 20);
  const uint8_t offsetMinute = recorderCheckpointTwoDigits(value, 23);
  return offsetHour <= 14U && offsetMinute <= 59U &&
      (offsetHour != 14U || offsetMinute == 0U);
}

inline void finalizeRecorderCheckpoint(StoredRecorderCheckpoint &checkpoint) {
  checkpoint.crc32 = recorderCheckpointCrc32(
      reinterpret_cast<const uint8_t *>(&checkpoint),
      offsetof(StoredRecorderCheckpoint, crc32));
}

inline bool initializeRecorderCheckpoint(StoredRecorderCheckpoint &checkpoint,
                                         const char *capsuleId,
                                         const char *createdAt) {
  memset(&checkpoint, 0, sizeof(checkpoint));
  checkpoint.magic = kRecorderCheckpointMagic;
  checkpoint.version = kRecorderCheckpointVersion;
  checkpoint.state = static_cast<uint8_t>(RecorderCheckpointState::recording);
  checkpoint.sampleRateHz = kCapsuleSampleRate;
  checkpoint.channels = kCapsuleChannels;
  checkpoint.bitsPerSample = kCapsuleBitsPerSample;
  if (!recorderCheckpointCopy(checkpoint.capsuleId,
                              sizeof(checkpoint.capsuleId), capsuleId) ||
      !recorderCheckpointCopy(checkpoint.createdAt,
                              sizeof(checkpoint.createdAt), createdAt) ||
      !recorderCheckpointUuidShape(checkpoint.capsuleId) ||
      !recorderCheckpointCreatedAtShape(checkpoint.createdAt)) {
    memset(&checkpoint, 0, sizeof(checkpoint));
    return false;
  }
  finalizeRecorderCheckpoint(checkpoint);
  return true;
}

inline void updateRecorderCheckpoint(StoredRecorderCheckpoint &checkpoint,
                                     uint32_t confirmedDataBytes,
                                     uint32_t audioCrc32) {
  checkpoint.confirmedDataBytes = confirmedDataBytes & ~1U;
  checkpoint.audioCrc32 = audioCrc32;
  finalizeRecorderCheckpoint(checkpoint);
}

inline void failRecorderCheckpoint(StoredRecorderCheckpoint &checkpoint,
                                   RecorderFailureStage stage) {
  checkpoint.state = static_cast<uint8_t>(RecorderCheckpointState::failed);
  checkpoint.failureStage = static_cast<uint8_t>(stage);
  finalizeRecorderCheckpoint(checkpoint);
}

inline bool validateRecorderCheckpoint(
    const StoredRecorderCheckpoint &checkpoint) {
  const uint8_t state = checkpoint.state;
  const bool recordingState =
      state == static_cast<uint8_t>(RecorderCheckpointState::recording);
  const bool failedState =
      state == static_cast<uint8_t>(RecorderCheckpointState::failed);
  const uint8_t maximumStage =
      static_cast<uint8_t>(RecorderFailureStage::captureIncomplete);
  return checkpoint.magic == kRecorderCheckpointMagic &&
      checkpoint.version == kRecorderCheckpointVersion &&
      (recordingState || failedState) &&
      checkpoint.failureStage <= maximumStage &&
      ((recordingState && checkpoint.failureStage == 0) ||
       (failedState && checkpoint.failureStage > 0)) &&
      checkpoint.confirmedDataBytes <= kMaximumRecordingAudioBytes &&
      (checkpoint.confirmedDataBytes & 1U) == 0 &&
      checkpoint.sampleRateHz == kCapsuleSampleRate &&
      checkpoint.channels == kCapsuleChannels &&
      checkpoint.bitsPerSample == kCapsuleBitsPerSample &&
      recorderCheckpointHasTerminator(checkpoint.capsuleId,
                                      sizeof(checkpoint.capsuleId)) &&
      recorderCheckpointHasTerminator(checkpoint.createdAt,
                                      sizeof(checkpoint.createdAt)) &&
      recorderCheckpointUuidShape(checkpoint.capsuleId) &&
      recorderCheckpointCreatedAtShape(checkpoint.createdAt) &&
      checkpoint.crc32 == recorderCheckpointCrc32(
          reinterpret_cast<const uint8_t *>(&checkpoint),
          offsetof(StoredRecorderCheckpoint, crc32));
}

constexpr bool recorderCheckpointDue(uint32_t previouslyConfirmedBytes,
                                     uint32_t currentDataBytes) {
  return currentDataBytes >= previouslyConfirmedBytes &&
      currentDataBytes - previouslyConfirmedBytes >=
          kRecorderCheckpointIntervalBytes;
}

enum class RecorderRecoveryDisposition : uint8_t {
  damaged = 0,
  noAudio,
  preserveFailure,
  recoverConfirmedAudio,
};

struct RecorderRecoveryPlan {
  RecorderRecoveryDisposition disposition =
      RecorderRecoveryDisposition::damaged;
  uint32_t recoveredDataBytes = 0;
};

inline RecorderRecoveryPlan planRecorderRecovery(
    const StoredRecorderCheckpoint &checkpoint, uint32_t actualDataBytes,
    uint32_t confirmedAudioCrc32) {
  const uint32_t alignedActualBytes = actualDataBytes & ~1U;
  if (!validateRecorderCheckpoint(checkpoint) ||
      alignedActualBytes > kMaximumRecordingAudioBytes ||
      alignedActualBytes < checkpoint.confirmedDataBytes ||
      confirmedAudioCrc32 != checkpoint.audioCrc32) {
    return {};
  }
  if (checkpoint.state ==
      static_cast<uint8_t>(RecorderCheckpointState::failed)) {
    return {RecorderRecoveryDisposition::preserveFailure,
            checkpoint.confirmedDataBytes};
  }
  if (checkpoint.confirmedDataBytes == 0) {
    return {RecorderRecoveryDisposition::noAudio, 0};
  }
  return {RecorderRecoveryDisposition::recoverConfirmedAudio,
          checkpoint.confirmedDataBytes};
}

inline RecorderRecoveryPlan planRecorderRecovery(
    const StoredRecorderCheckpoint &checkpoint, uint32_t actualDataBytes) {
  return planRecorderRecovery(checkpoint, actualDataBytes,
                              checkpoint.audioCrc32);
}

}  // namespace pokepod
