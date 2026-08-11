#pragma once

#include <stddef.h>
#include <stdint.h>

#include "AudioBoardProfile.h"
#include "VoiceConditioner.h"

namespace pokepod {

enum class AudioInputChannel : uint8_t { undecided, left, right };

inline const char *audioInputChannelName(AudioInputChannel channel) {
  switch (channel) {
    case AudioInputChannel::left: return "left";
    case AudioInputChannel::right: return "right";
    case AudioInputChannel::undecided: return "undecided";
  }
  return "undecided";
}

struct AudioFrontEndMetrics : public VoiceConditionerMetrics {
  AudioDspProfile profile = AudioDspProfile::unavailable;
  AudioInputChannel selectedChannel = AudioInputChannel::undecided;
  uint64_t inputFrames = 0;
  uint64_t leftEnergy = 0;
  uint64_t rightEnergy = 0;
  uint32_t clippedInputSamples = 0;
  uint16_t leftPeak = 0;
  uint16_t rightPeak = 0;
};

// Shared capsule/BLE voice front end. It performs a short deterministic slot
// probe, a 79-tap anti-alias FIR and 3:1 decimation. The independent
// VoiceConditioner owns the 16 kHz speech cleanup and dynamics. No heap
// allocation is used.
class AudioFrontEnd {
 public:
  static constexpr size_t kSelectionFrames = 96;
  static constexpr size_t kFirTaps = 79;
  static constexpr int32_t kLimiter = VoiceConditioner::kLimiter;

  void configure(AudioDspProfile profile) {
    profile_ = profile;
    const AudioBoardProfile boardProfile = profile == AudioDspProfile::v2Baseline
        ? audioBoardProfile(BoardVariant::v2Co5300Cst820)
        : audioBoardProfile(BoardVariant::v1Sh8601Ft3168);
    rightChannelEnergyRatioQ8_ = boardProfile.rightChannelEnergyRatioQ8;
    channelSelectionMinimumPeak_ = boardProfile.channelSelectionMinimumPeak;
  }

  void reset() {
    selectedChannel_ = AudioInputChannel::undecided;
    selectionUsed_ = 0;
    leftSelectionEnergy_ = 0;
    rightSelectionEnergy_ = 0;
    ringIndex_ = 0;
    sampleCount_ = 0;
    decimationPhase_ = 0;
    metrics_ = {};
    conditioner_.reset(&metrics_);
    metrics_.profile = profile_;
    for (auto &sample : selection_) sample = 0;
    for (auto &sample : ring_) sample = 0;
  }

  size_t processStereo16(const uint8_t *stereo, size_t stereoBytes,
                         uint8_t *mono, size_t monoCapacity) {
    if (stereo == nullptr || mono == nullptr || monoCapacity < 2) return 0;
    const size_t frames = stereoBytes / 4;
    size_t outputBytes = 0;
    for (size_t frame = 0; frame < frames; ++frame) {
      const int16_t left = decode(stereo + frame * 4);
      const int16_t right = decode(stereo + frame * 4 + 2);
      observeInput(left, right);
      if (selectedChannel_ == AudioInputChannel::undecided) {
        selection_[selectionUsed_ * 2] = left;
        selection_[selectionUsed_ * 2 + 1] = right;
        leftSelectionEnergy_ += magnitude(left);
        rightSelectionEnergy_ += magnitude(right);
        ++selectionUsed_;
        if (selectionUsed_ == kSelectionFrames) {
          lockChannel();
          for (size_t index = 0; index < selectionUsed_; ++index) {
            const int16_t selected = selectedChannel_ == AudioInputChannel::right
                ? selection_[index * 2 + 1] : selection_[index * 2];
            if (!filterSample(selected, mono, monoCapacity, outputBytes)) {
              return outputBytes;
            }
          }
        }
        continue;
      }
      const int16_t selected = selectedChannel_ == AudioInputChannel::right
          ? right : left;
      if (!filterSample(selected, mono, monoCapacity, outputBytes)) break;
    }
    return outputBytes;
  }

  const AudioFrontEndMetrics &metrics() const { return metrics_; }
  AudioInputChannel selectedChannel() const { return selectedChannel_; }

 private:
  static constexpr int16_t kFirQ15[kFirTaps] = {
      0, 0, 0, -1, -3, -3, 1, 9, 14, 7, -11, -32, -35, -7, 43, 79,
      64, -15, -116, -161, -88, 89, 259, 277, 73, -265, -504, -415,
      45, 631, 914, 549, -420, -1459, -1750, -647, 1886, 5116, 7821,
      8878, 7821, 5116, 1886, -647, -1750, -1459, -420, 549, 914, 631,
      45, -415, -504, -265, 73, 277, 259, 89, -88, -161, -116, -15,
      64, 79, 43, -7, -35, -32, -11, 7, 14, 9, 1, -3, -3, -1, 0, 0, 0};
  static int16_t decode(const uint8_t *bytes) {
    return static_cast<int16_t>(static_cast<uint16_t>(bytes[0]) |
                                static_cast<uint16_t>(bytes[1]) << 8);
  }

  static uint16_t magnitude(int16_t value) {
    const int32_t wide = value;
    return static_cast<uint16_t>(wide < 0 ? -wide : wide);
  }

  void observeInput(int16_t left, int16_t right) {
    const uint16_t leftMagnitude = magnitude(left);
    const uint16_t rightMagnitude = magnitude(right);
    ++metrics_.inputFrames;
    metrics_.leftEnergy += static_cast<uint64_t>(leftMagnitude) * leftMagnitude;
    metrics_.rightEnergy += static_cast<uint64_t>(rightMagnitude) * rightMagnitude;
    if (leftMagnitude > metrics_.leftPeak) metrics_.leftPeak = leftMagnitude;
    if (rightMagnitude > metrics_.rightPeak) metrics_.rightPeak = rightMagnitude;
    if (leftMagnitude >= 32760) ++metrics_.clippedInputSamples;
    if (rightMagnitude >= 32760) ++metrics_.clippedInputSamples;
  }

  void lockChannel() {
    // The board normally mirrors or left-aligns its mono ADC. Select right only
    // when its short-window energy is decisively larger, keeping left as the
    // deterministic tie/default channel.
    const uint64_t rightThreshold =
        leftSelectionEnergy_ * rightChannelEnergyRatioQ8_ / 256U;
    selectedChannel_ = rightSelectionEnergy_ > rightThreshold &&
        metrics_.rightPeak >= channelSelectionMinimumPeak_
        ? AudioInputChannel::right : AudioInputChannel::left;
    metrics_.selectedChannel = selectedChannel_;
  }

  bool filterSample(int16_t sample, uint8_t *mono, size_t monoCapacity,
                    size_t &outputBytes) {
    ring_[ringIndex_] = sample;
    ringIndex_ = (ringIndex_ + 1) % kFirTaps;
    ++sampleCount_;
    decimationPhase_ = static_cast<uint8_t>((decimationPhase_ + 1) % 3);
    if (sampleCount_ < kFirTaps || decimationPhase_ != 0) return true;
    if (outputBytes + 2 > monoCapacity) return false;

    int64_t accumulator = 0;
    size_t index = ringIndex_;
    for (size_t tap = 0; tap < kFirTaps; ++tap) {
      accumulator += static_cast<int32_t>(ring_[index]) * kFirQ15[tap];
      index = (index + 1) % kFirTaps;
    }
    int32_t filtered = static_cast<int32_t>(
        (accumulator + (accumulator >= 0 ? 16384 : -16384)) / 32768);
    filtered = conditioner_.process(filtered);
    const uint16_t encoded = static_cast<uint16_t>(
        static_cast<int16_t>(filtered));
    mono[outputBytes++] = static_cast<uint8_t>(encoded & 0xff);
    mono[outputBytes++] = static_cast<uint8_t>((encoded >> 8) & 0xff);
    return true;
  }

  AudioInputChannel selectedChannel_ = AudioInputChannel::undecided;
  AudioDspProfile profile_ = AudioDspProfile::v1Measured;
  uint16_t rightChannelEnergyRatioQ8_ = 512;
  uint16_t channelSelectionMinimumPeak_ = 32;
  int16_t selection_[kSelectionFrames * 2] = {};
  size_t selectionUsed_ = 0;
  uint64_t leftSelectionEnergy_ = 0;
  uint64_t rightSelectionEnergy_ = 0;
  int16_t ring_[kFirTaps] = {};
  size_t ringIndex_ = 0;
  uint64_t sampleCount_ = 0;
  uint8_t decimationPhase_ = 0;
  VoiceConditioner conditioner_;
  AudioFrontEndMetrics metrics_;
};

}  // namespace pokepod
