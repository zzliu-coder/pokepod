#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ImaAdpcm.h"

namespace pokepod {

constexpr uint8_t kBleVoiceVersion = 1;
constexpr uint16_t kBleVoiceSampleRate = 16000;
constexpr uint16_t kBleVoiceSamplesPerFrame = 320;
constexpr uint16_t kBleVoiceFrameDurationMs = 20;
constexpr size_t kBleVoiceHeaderBytes = 15;
constexpr size_t kBleVoicePayloadBytes = 160;
constexpr size_t kBleVoiceFrameBytes = kBleVoiceHeaderBytes + kBleVoicePayloadBytes;
constexpr uint16_t kBleVoiceMinimumMtu = 185;

constexpr const char *kBleVoiceServiceUuid =
    "7A530001-4B50-4F44-9000-504F4B45504F";
constexpr const char *kBleVoiceDeviceInfoUuid =
    "7A530002-4B50-4F44-9000-504F4B45504F";
constexpr const char *kBleVoiceCommandUuid =
    "7A530003-4B50-4F44-9000-504F4B45504F";
constexpr const char *kBleVoiceEventUuid =
    "7A530004-4B50-4F44-9000-504F4B45504F";
constexpr const char *kBleVoiceAudioUuid =
    "7A530005-4B50-4F44-9000-504F4B45504F";

enum class BleVoiceCommandType : uint8_t {
  ready = 1,
  reject = 2,
  stopAck = 3,
  ping = 4,
};

enum class BleVoiceEventType : uint8_t {
  sessionStart = 1,
  sessionEnd = 2,
  status = 3,
  error = 4,
};

struct BleVoiceControl {
  uint8_t version = kBleVoiceVersion;
  uint8_t type = 0;
  uint32_t sessionId = 0;
  uint16_t code = 0;
};

struct BleVoiceAudioFrame {
  uint8_t bytes[kBleVoiceFrameBytes] = {};
};

inline void writeVoiceU16(uint8_t *output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8);
}

inline void writeVoiceU32(uint8_t *output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8);
  output[2] = static_cast<uint8_t>(value >> 16);
  output[3] = static_cast<uint8_t>(value >> 24);
}

inline uint16_t readVoiceU16(const uint8_t *input) {
  return static_cast<uint16_t>(input[0]) |
      static_cast<uint16_t>(input[1]) << 8;
}

inline uint32_t readVoiceU32(const uint8_t *input) {
  return static_cast<uint32_t>(input[0]) |
      static_cast<uint32_t>(input[1]) << 8 |
      static_cast<uint32_t>(input[2]) << 16 |
      static_cast<uint32_t>(input[3]) << 24;
}

inline size_t encodeBleVoiceControl(const BleVoiceControl &control,
                                    uint8_t *output, size_t capacity) {
  if (output == nullptr || capacity < 8) return 0;
  output[0] = control.version;
  output[1] = control.type;
  writeVoiceU32(output + 2, control.sessionId);
  writeVoiceU16(output + 6, control.code);
  return 8;
}

inline bool decodeBleVoiceControl(const uint8_t *input, size_t length,
                                  BleVoiceControl &control) {
  if (input == nullptr || length != 8 || input[0] != kBleVoiceVersion) {
    return false;
  }
  control.version = input[0];
  control.type = input[1];
  control.sessionId = readVoiceU32(input + 2);
  control.code = readVoiceU16(input + 6);
  return control.type >= static_cast<uint8_t>(BleVoiceCommandType::ready) &&
      control.type <= static_cast<uint8_t>(BleVoiceCommandType::ping);
}

inline bool encodeBleVoiceAudio(uint32_t sessionId, uint32_t sequence,
                                const int16_t *samples,
                                BleVoiceAudioFrame &frame,
                                uint8_t flags = 0) {
  if (samples == nullptr) return false;
  ImaAdpcmState initial;
  const size_t encoded = imaAdpcmEncodeIndependent(
      samples, kBleVoiceSamplesPerFrame, frame.bytes + kBleVoiceHeaderBytes,
      kBleVoicePayloadBytes, initial);
  if (encoded != kBleVoicePayloadBytes) return false;
  frame.bytes[0] = kBleVoiceVersion;
  frame.bytes[1] = flags;
  writeVoiceU32(frame.bytes + 2, sessionId);
  writeVoiceU32(frame.bytes + 6, sequence);
  writeVoiceU16(frame.bytes + 10, kBleVoiceSamplesPerFrame);
  writeVoiceU16(frame.bytes + 12, static_cast<uint16_t>(initial.predictor));
  frame.bytes[14] = initial.stepIndex;
  return true;
}

inline bool decodeBleVoiceAudio(const uint8_t *input, size_t length,
                                uint32_t &sessionId, uint32_t &sequence,
                                int16_t *samples, size_t sampleCapacity) {
  if (input == nullptr || samples == nullptr || length != kBleVoiceFrameBytes ||
      input[0] != kBleVoiceVersion ||
      readVoiceU16(input + 10) != kBleVoiceSamplesPerFrame ||
      input[14] > 88 || sampleCapacity < kBleVoiceSamplesPerFrame) {
    return false;
  }
  sessionId = readVoiceU32(input + 2);
  sequence = readVoiceU32(input + 6);
  ImaAdpcmState initial;
  initial.predictor = static_cast<int16_t>(readVoiceU16(input + 12));
  initial.stepIndex = input[14];
  return imaAdpcmDecodeIndependent(
      input + kBleVoiceHeaderBytes, kBleVoicePayloadBytes, initial, samples,
      kBleVoiceSamplesPerFrame) == kBleVoiceSamplesPerFrame;
}

template <size_t Capacity>
class BleVoiceFrameQueue {
 public:
  static_assert(Capacity > 0, "BLE voice queue must be bounded");

  bool push(const BleVoiceAudioFrame &frame) {
    if (count_ == Capacity) {
      ++overflowCount_;
      return false;
    }
    frames_[tail_] = frame;
    tail_ = (tail_ + 1) % Capacity;
    ++count_;
    return true;
  }

  bool pop(BleVoiceAudioFrame &frame) {
    if (count_ == 0) return false;
    frame = frames_[head_];
    head_ = (head_ + 1) % Capacity;
    --count_;
    return true;
  }

  void clear() { head_ = tail_ = count_ = 0; }
  size_t size() const { return count_; }
  constexpr size_t capacity() const { return Capacity; }
  uint32_t overflowCount() const { return overflowCount_; }

 private:
  BleVoiceAudioFrame frames_[Capacity] = {};
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t count_ = 0;
  uint32_t overflowCount_ = 0;
};

constexpr bool bleVoiceMtuReady(uint16_t mtu) {
  return mtu >= kBleVoiceMinimumMtu;
}

constexpr bool bleVoiceCommandTargetsSession(uint32_t commandSessionId,
                                             uint32_t activeSessionId) {
  return commandSessionId != 0 && commandSessionId == activeSessionId;
}

}  // namespace pokepod
