#include "AudioCaptureRuntime.h"

#include <esp_timer.h>

namespace pokepod {

AudioCaptureReadResult AudioCaptureRuntime::PipelineSource::readStereo48(
    uint8_t *output, size_t capacity, uint32_t timeoutMs) {
  (void)timeoutMs;
  AudioCaptureReadResult result;
  const int64_t startedUs = esp_timer_get_time();
  result.bytes = audio_ == nullptr
      ? 0 : audio_->readCaptureRealtime(output, capacity);
  result.elapsedUs = static_cast<uint32_t>(esp_timer_get_time() - startedUs);
  result.status = result.bytes == 0 ? AudioCaptureReadStatus::timeout
                                    : AudioCaptureReadStatus::ok;
  return result;
}

bool AudioCaptureRuntime::begin(BoardVariant variant, Print &log) {
  if (ready_) return true;
  const AudioBoardProfile profile = audioBoardProfile(variant);
  if (!profile.valid() || !service_.configureDspProfile(profile.dsp)) {
    log.println("{\"event\":\"capture_task\",\"ok\":false,\"stage\":\"profile\"}");
    return false;
  }
  profile_ = profile.dsp;
  stopped_ = xSemaphoreCreateBinary();
  if (stopped_ == nullptr) {
    log.println("{\"event\":\"capture_task\",\"ok\":false,\"stage\":\"semaphore\"}");
    return false;
  }
  const BaseType_t created = xTaskCreate(
      taskThunk, "pokepod_capture", kTaskStackBytes, this, kTaskPriority,
      &task_);
  if (created != pdPASS || task_ == nullptr) {
    vSemaphoreDelete(stopped_);
    stopped_ = nullptr;
    log.println("{\"event\":\"capture_task\",\"ok\":false,\"stage\":\"task\"}");
    return false;
  }
  ready_ = true;
  log.printf(
      "{\"event\":\"capture_task\",\"ok\":true,\"priority\":%u,\"stack_bytes\":%u,\"ring_frames\":%u,\"dsp_profile\":\"%s\"}\n",
      static_cast<unsigned>(kTaskPriority),
      static_cast<unsigned>(kTaskStackBytes),
      static_cast<unsigned>(kRingFrames), audioDspProfileName(profile_));
  return true;
}

bool AudioCaptureRuntime::start(AudioPipeline &audio, uint32_t sessionId,
                                Print &log) {
  if (!ready_ || running() || sessionId == 0) return false;
  while (xSemaphoreTake(stopped_, 0) == pdTRUE) {}
  if (!audio.startCapture(log)) return false;
  audio_ = &audio;
  source_.bind(audio_);
  if (!service_.startSession(sessionId, source_)) {
    source_.bind(nullptr);
    audio_ = nullptr;
    audio.stopHardware(log);
    return false;
  }
  incomplete_.store(false, std::memory_order_release);
  stopRequested_.store(false, std::memory_order_release);
  active_.store(true, std::memory_order_release);
  xTaskNotifyGive(task_);
  return true;
}

bool AudioCaptureRuntime::stop(Print &log) {
  if (!running()) return true;
  stopRequested_.store(true, std::memory_order_release);
  xTaskNotifyGive(task_);
  if (xSemaphoreTake(stopped_, pdMS_TO_TICKS(kStopTimeoutMs)) != pdTRUE) {
    incomplete_.store(true, std::memory_order_release);
    log.println("{\"event\":\"capture_task_stop\",\"ok\":false,\"stage\":\"timeout\"}");
    return false;
  }
  service_.stopSession();
  source_.bind(nullptr);
  AudioPipeline *audio = audio_;
  audio_ = nullptr;
  if (audio != nullptr) audio->stopHardware(log);
  log.printf(
      "{\"event\":\"capture_task_stop\",\"ok\":true,\"incomplete\":%s,\"stack_high_water\":%u}\n",
      incomplete() ? "true" : "false",
      static_cast<unsigned>(taskStackHighWater()));
  return true;
}

UBaseType_t AudioCaptureRuntime::taskStackHighWater() const {
  return task_ == nullptr ? 0 : uxTaskGetStackHighWaterMark(task_);
}

void AudioCaptureRuntime::taskThunk(void *context) {
  static_cast<AudioCaptureRuntime *>(context)->taskMain();
}

void AudioCaptureRuntime::taskMain() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (active_.load(std::memory_order_acquire)) {
      if (stopRequested_.load(std::memory_order_acquire)) break;
      const AudioCaptureCycleResult cycle = service_.captureOnce(millis());
      if (cycle == AudioCaptureCycleResult::frameDropped ||
          cycle == AudioCaptureCycleResult::sourceTimeout ||
          cycle == AudioCaptureCycleResult::sourceOverrun ||
          cycle == AudioCaptureCycleResult::sourceFailure) {
        incomplete_.store(true, std::memory_order_release);
      }
      if (cycle == AudioCaptureCycleResult::sourceFailure) taskYIELD();
    }
    if (active_.exchange(false, std::memory_order_acq_rel)) {
      xSemaphoreGive(stopped_);
    }
  }
}

}  // namespace pokepod
