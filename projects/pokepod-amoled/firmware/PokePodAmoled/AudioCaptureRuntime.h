#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "AudioCaptureService.h"
#include "AudioPipeline.h"

namespace pokepod {

// Owns the firmware's sole realtime microphone task. Hardware start/stop and
// all logging remain on the Arduino loop; the task only performs bounded I2S
// reads, fixed-buffer DSP and SPSC publication.
class AudioCaptureRuntime {
 public:
  static constexpr size_t kRingFrames = 6;
  static constexpr uint32_t kTaskStackBytes = 3072;
  static constexpr UBaseType_t kTaskPriority = 5;
  static constexpr uint32_t kStopTimeoutMs = 500;

  bool begin(BoardVariant variant, Print &log);
  bool start(AudioPipeline &audio, uint32_t sessionId, Print &log);
  bool stop(Print &log);
  bool pop(AudioCaptureFrame &frame) { return service_.pop(frame); }

  bool ready() const { return ready_; }
  bool running() const {
    return active_.load(std::memory_order_acquire);
  }
  bool incomplete() const {
    return incomplete_.load(std::memory_order_acquire);
  }
  AudioCaptureServiceMetrics metrics() const { return service_.metrics(); }
  UBaseType_t taskStackHighWater() const;

 private:
  class PipelineSource final : public AudioCaptureSource {
   public:
    void bind(AudioPipeline *audio) { audio_ = audio; }
    bool start() override { return audio_ != nullptr && audio_->active(); }
    void stop() override {}
    AudioCaptureReadResult readStereo48(uint8_t *output, size_t capacity,
                                        uint32_t timeoutMs) override;

   private:
    AudioPipeline *audio_ = nullptr;
  };

  static void taskThunk(void *context);
  void taskMain();

  AudioCaptureService<kRingFrames> service_;
  PipelineSource source_;
  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t stopped_ = nullptr;
  AudioPipeline *audio_ = nullptr;
  std::atomic<bool> active_{false};
  std::atomic<bool> stopRequested_{false};
  std::atomic<bool> incomplete_{false};
  bool ready_ = false;
  AudioDspProfile profile_ = AudioDspProfile::unavailable;
};

}  // namespace pokepod
