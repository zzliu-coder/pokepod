#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "AudioCaptureService.h"
#include "AudioCaptureSessionState.h"
#include "AudioPipeline.h"

namespace pokepod {

// Owns the firmware's sole realtime microphone task. Hardware start/stop and
// all logging remain on the Arduino loop; the task only performs bounded I2S
// reads, fixed-buffer DSP and SPSC publication.
class AudioCaptureRuntime {
 public:
  // 64 x 20 ms retains 1.28 s of fixed-size mono frames. This covers the
  // admitted 1 s storage-tail budget while the loop drains an SD write or a
  // durable checkpoint transaction, without heap growth during capture.
  static constexpr size_t kRingFrames = 64;
  static_assert(kRingFrames * sizeof(AudioCaptureFrame) <= 48U * 1024U,
                "capture ring must remain within the fixed RAM budget");
  static constexpr uint32_t kTaskStackBytes = 3072;
  static constexpr UBaseType_t kTaskPriority = 5;
  static constexpr uint32_t kStopTimeoutMs = 500;

  bool begin(BoardVariant variant, Print &log);
  bool start(AudioPipeline &audio, uint32_t sessionId, Print &log);
  bool stop(Print &log);
  bool pollFinalize(Print &log);
  bool pop(AudioCaptureFrame &frame) { return service_.pop(frame); }

  bool ready() const { return ready_; }
  bool running() const { return sessionState_.busy(); }
  bool finalizePending() const {
    return sessionState_.finalizePending() || sessionState_.stopRequested();
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
    // Arduino-ESP32 ESP_I2S::readBytes exposes only the byte count. The
    // adapter cannot truthfully distinguish RX DMA overrun from short/zero
    // reads, so diagnostics must report this capability as unavailable.
    bool overrunObservable() const override { return false; }
    AudioCaptureReadResult readStereo48(uint8_t *output, size_t capacity,
                                        uint32_t timeoutMs) override;

   private:
    AudioPipeline *audio_ = nullptr;
  };

  static void taskThunk(void *context);
  void taskMain();
  bool finalizeStoppedSession(Print &log);

  AudioCaptureService<kRingFrames> service_;
  PipelineSource source_;
  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t stopped_ = nullptr;
  AudioPipeline *audio_ = nullptr;
  AudioCaptureSessionState sessionState_;
  std::atomic<bool> incomplete_{false};
  bool ready_ = false;
  AudioDspProfile profile_ = AudioDspProfile::unavailable;
};

}  // namespace pokepod
