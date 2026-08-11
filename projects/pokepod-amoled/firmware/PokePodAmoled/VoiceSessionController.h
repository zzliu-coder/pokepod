#pragma once

#include <stddef.h>
#include <stdint.h>

#include "AudioCaptureRouter.h"
#include "AudioFrontEnd.h"
#include "BleVoiceProtocol.h"

namespace pokepod {

enum class VoiceSessionState : uint8_t {
  idle,
  waitingForReady,
  streaming,
  ending,
  awaitingStopAck,
  failed,
};

enum class VoiceSessionError : uint8_t {
  none,
  notConnected,
  mtuTooSmall,
  microphoneBusy,
  queueOverflow,
  readyTimeout,
  stopAckTimeout,
  streamTimeout,
  notifyFailed,
  disconnected,
};

class VoiceSessionController {
 public:
  static constexpr uint32_t kReadyTimeoutMs = 400;
  static constexpr uint32_t kStopAckTimeoutMs = 1000;
  static constexpr size_t kQueueFrames = 24;

  bool begin(uint32_t sessionId, uint32_t nowMs, bool connected, uint16_t mtu,
             AudioCaptureRouter &router) {
    if (state_ != VoiceSessionState::idle &&
        state_ != VoiceSessionState::failed) {
      return false;
    }
    if (state_ == VoiceSessionState::failed) complete();
    if (!connected) return fail(VoiceSessionError::notConnected);
    if (!bleVoiceMtuReady(mtu)) return fail(VoiceSessionError::mtuTooSmall);
    if (!router.acquire(AudioCaptureOwner::wirelessVoice)) {
      return fail(VoiceSessionError::microphoneBusy);
    }
    router_ = &router;
    state_ = VoiceSessionState::waitingForReady;
    error_ = VoiceSessionError::none;
    sessionId_ = sessionId;
    sequence_ = 0;
    pcmUsed_ = 0;
    stopRequested_ = false;
    startedAtMs_ = nowMs;
    lastAudioAtMs_ = nowMs;
    sessionEndSentAtMs_ = 0;
    audioFrontEnd_.reset();
    queue_.clear();
    return true;
  }

  bool markReady(uint32_t sessionId, uint32_t nowMs) {
    if (state_ != VoiceSessionState::waitingForReady ||
        sessionId != sessionId_) return false;
    state_ = stopRequested_ ? VoiceSessionState::ending
                            : VoiceSessionState::streaming;
    lastAudioAtMs_ = nowMs;
    return true;
  }

  bool appendStereo48(const uint8_t *data, size_t bytes, uint32_t nowMs) {
    if ((state_ != VoiceSessionState::waitingForReady &&
         state_ != VoiceSessionState::streaming) || stopRequested_ ||
        data == nullptr) return false;
    uint8_t monoBytes[192];
    size_t offset = 0;
    while (offset + 4 <= bytes) {
      size_t chunk = bytes - offset;
      if (chunk > 192) chunk = 192;
      chunk -= chunk % 4;
      const size_t converted = audioFrontEnd_.processStereo16(
          data + offset, chunk, monoBytes, sizeof(monoBytes));
      for (size_t monoOffset = 0; monoOffset + 1 < converted; monoOffset += 2) {
        const int16_t sample = static_cast<int16_t>(
            static_cast<uint16_t>(monoBytes[monoOffset]) |
            static_cast<uint16_t>(monoBytes[monoOffset + 1]) << 8);
        if (!appendMonoSamples(&sample, 1)) return false;
      }
      offset += chunk;
    }
    lastAudioAtMs_ = nowMs;
    return true;
  }

  bool appendMono16(const int16_t *samples, size_t count, uint32_t nowMs) {
    if ((state_ != VoiceSessionState::waitingForReady &&
         state_ != VoiceSessionState::streaming) || stopRequested_ ||
        samples == nullptr || count == 0) {
      return false;
    }
    if (!appendMonoSamples(samples, count)) return false;
    lastAudioAtMs_ = nowMs;
    return true;
  }

  bool takeFrame(BleVoiceAudioFrame &frame) {
    if (state_ != VoiceSessionState::streaming &&
        state_ != VoiceSessionState::ending) return false;
    return queue_.pop(frame);
  }

  bool peekFrame(BleVoiceAudioFrame &frame) const {
    if (state_ != VoiceSessionState::streaming &&
        state_ != VoiceSessionState::ending) return false;
    return queue_.peek(frame);
  }

  bool commitFrame(uint32_t expectedSequence) {
    BleVoiceAudioFrame frame;
    if (!peekFrame(frame) ||
        readVoiceU32(frame.bytes + 6) != expectedSequence) return false;
    return queue_.commit();
  }

  void abort(VoiceSessionError error) {
    if (error != VoiceSessionError::none) fail(error);
  }

  void end() {
    if (state_ == VoiceSessionState::idle ||
        state_ == VoiceSessionState::failed ||
        state_ == VoiceSessionState::ending) return;
    if (!queuePartialFrame()) return;
    stopRequested_ = true;
    // A fast release before ready keeps the captured voice, but the queue is
    // unreadable until the matching per-session ready arrives.
    if (state_ == VoiceSessionState::streaming) {
      state_ = VoiceSessionState::ending;
    }
    releaseCapture();
  }

  void complete() {
    releaseCapture();
    state_ = VoiceSessionState::idle;
    error_ = VoiceSessionError::none;
    pcmUsed_ = 0;
    stopRequested_ = false;
    sessionEndSentAtMs_ = 0;
    queue_.clear();
  }

  bool markSessionEndSent(uint32_t nowMs) {
    if (state_ != VoiceSessionState::ending || queue_.size() != 0) {
      return false;
    }
    state_ = VoiceSessionState::awaitingStopAck;
    sessionEndSentAtMs_ = nowMs;
    return true;
  }

  bool poll(uint32_t nowMs) {
    if (state_ == VoiceSessionState::waitingForReady &&
        nowMs - startedAtMs_ >= kReadyTimeoutMs) {
      return fail(VoiceSessionError::readyTimeout);
    }
    if (state_ == VoiceSessionState::streaming &&
        nowMs - lastAudioAtMs_ >= kReadyTimeoutMs) {
      return fail(VoiceSessionError::streamTimeout);
    }
    if (state_ == VoiceSessionState::awaitingStopAck &&
        nowMs - sessionEndSentAtMs_ >= kStopAckTimeoutMs) {
      error_ = VoiceSessionError::stopAckTimeout;
      state_ = VoiceSessionState::idle;
      queue_.clear();
      return false;
    }
    return state_ != VoiceSessionState::failed;
  }

  VoiceSessionState state() const { return state_; }
  VoiceSessionError error() const { return error_; }
  uint32_t sessionId() const { return sessionId_; }
  uint32_t nextSequence() const { return sequence_; }
  size_t queuedFrames() const { return queue_.size(); }
  uint32_t overflowCount() const { return queue_.overflowCount(); }
  bool active() const {
    return state_ == VoiceSessionState::waitingForReady ||
        state_ == VoiceSessionState::streaming ||
        state_ == VoiceSessionState::ending ||
        state_ == VoiceSessionState::awaitingStopAck;
  }
  bool acceptsAudio() const {
    return !stopRequested_ &&
        (state_ == VoiceSessionState::waitingForReady ||
         state_ == VoiceSessionState::streaming);
  }
  bool acceptsStopAck(uint32_t sessionId) const {
    return state_ == VoiceSessionState::awaitingStopAck &&
        bleVoiceCommandTargetsSession(sessionId, sessionId_);
  }

 private:
  bool appendMonoSamples(const int16_t *samples, size_t count) {
    for (size_t index = 0; index < count; ++index) {
      pcm_[pcmUsed_++] = samples[index];
      if (pcmUsed_ != kBleVoiceSamplesPerFrame) continue;
      BleVoiceAudioFrame frame;
      if (!encodeBleVoiceAudio(sessionId_, sequence_++, pcm_, frame) ||
          !queue_.push(frame)) {
        return fail(VoiceSessionError::queueOverflow);
      }
      pcmUsed_ = 0;
    }
    return true;
  }

  bool fail(VoiceSessionError error) {
    error_ = error;
    state_ = VoiceSessionState::failed;
    pcmUsed_ = 0;
    stopRequested_ = false;
    queue_.clear();
    releaseCapture();
    return false;
  }

  bool queuePartialFrame() {
    if (pcmUsed_ == 0) return true;
    for (size_t index = pcmUsed_; index < kBleVoiceSamplesPerFrame; ++index) {
      pcm_[index] = 0;
    }
    BleVoiceAudioFrame frame;
    if (!encodeBleVoiceAudio(sessionId_, sequence_++, pcm_, frame) ||
        !queue_.push(frame)) {
      return fail(VoiceSessionError::queueOverflow);
    }
    pcmUsed_ = 0;
    return true;
  }

  void releaseCapture() {
    if (router_ != nullptr) {
      router_->release(AudioCaptureOwner::wirelessVoice);
      router_ = nullptr;
    }
  }

  VoiceSessionState state_ = VoiceSessionState::idle;
  VoiceSessionError error_ = VoiceSessionError::none;
  AudioCaptureRouter *router_ = nullptr;
  AudioFrontEnd audioFrontEnd_;
  BleVoiceFrameQueue<kQueueFrames> queue_;
  int16_t pcm_[kBleVoiceSamplesPerFrame] = {};
  size_t pcmUsed_ = 0;
  bool stopRequested_ = false;
  uint32_t sessionId_ = 0;
  uint32_t sequence_ = 0;
  uint32_t startedAtMs_ = 0;
  uint32_t lastAudioAtMs_ = 0;
  uint32_t sessionEndSentAtMs_ = 0;
};

}  // namespace pokepod
