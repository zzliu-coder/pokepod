#pragma once

#include <stdint.h>

namespace pokepod {

enum class AudioCaptureOwner : uint8_t { none, localCapsule, wirelessVoice };

class AudioCaptureRouter {
 public:
  bool acquire(AudioCaptureOwner owner) {
    if (owner == AudioCaptureOwner::none) return false;
    if (owner_ != AudioCaptureOwner::none && owner_ != owner) return false;
    owner_ = owner;
    return true;
  }

  void release(AudioCaptureOwner owner) {
    if (owner_ == owner) owner_ = AudioCaptureOwner::none;
  }

  AudioCaptureOwner owner() const { return owner_; }
  bool available() const { return owner_ == AudioCaptureOwner::none; }
  bool localRecording() const { return owner_ == AudioCaptureOwner::localCapsule; }
  bool wirelessStreaming() const { return owner_ == AudioCaptureOwner::wirelessVoice; }

 private:
  AudioCaptureOwner owner_ = AudioCaptureOwner::none;
};

}  // namespace pokepod
