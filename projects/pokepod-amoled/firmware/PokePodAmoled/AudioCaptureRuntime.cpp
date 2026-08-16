#include "AudioCaptureRuntime.h"

#include <esp_timer.h>

namespace pokepod {

AudioCaptureReadResult AudioCaptureRuntime::PipelineSource::readStereo48(
    uint8_t *output, size_t capacity) {
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
      "{\"event\":\"capture_task\",\"ok\":true,\"priority\":%u,\"stack_bytes\":%u,\"ring_frames\":%u,\"read_timeout_ms\":%u,\"stop_timeout_ms\":%u,\"dsp_profile\":\"%s\",\"source_overrun_observable\":false}\n",
      static_cast<unsigned>(kTaskPriority),
      static_cast<unsigned>(kTaskStackBytes),
      static_cast<unsigned>(kRingFrames),
      static_cast<unsigned>(kAudioCaptureReadTimeoutMs),
      static_cast<unsigned>(kAudioCaptureStopTimeoutMs),
      audioDspProfileName(profile_));
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
  if (!sessionState_.begin()) {
    service_.stopSession();
    source_.bind(nullptr);
    audio_ = nullptr;
    audio.stopHardware(log);
    return false;
  }
  incomplete_.store(false, std::memory_order_release);
  xTaskNotifyGive(task_);
  return true;
}

bool AudioCaptureRuntime::stop(Print &log) {
  if (!running()) return true;
  if (sessionState_.finalizePending()) {
    if (xSemaphoreTake(stopped_, pdMS_TO_TICKS(kStopTimeoutMs)) != pdTRUE) {
      return false;
    }
    if (!sessionState_.acknowledgeTaskStopped()) return false;
    return finalizeStoppedSession(log);
  }
  if (!sessionState_.requestStop()) return false;
  xTaskNotifyGive(task_);
  if (xSemaphoreTake(stopped_, pdMS_TO_TICKS(kStopTimeoutMs)) != pdTRUE) {
    incomplete_.store(true, std::memory_order_release);
    log.println("{\"event\":\"capture_task_stop\",\"ok\":false,\"stage\":\"timeout\"}");
    return false;
  }
  if (!sessionState_.acknowledgeTaskStopped()) return false;
  return finalizeStoppedSession(log);
}

bool AudioCaptureRuntime::pollFinalize(Print &log) {
  if (!sessionState_.finalizePending()) return !running();
  if (xSemaphoreTake(stopped_, 0) != pdTRUE) return false;
  if (!sessionState_.acknowledgeTaskStopped()) return false;
  return finalizeStoppedSession(log);
}

bool AudioCaptureRuntime::finalizeStoppedSession(Print &log) {
  if (!sessionState_.finalizePending()) return false;
  service_.stopSession();
  source_.bind(nullptr);
  AudioPipeline *audio = audio_;
  audio_ = nullptr;
  if (audio != nullptr) audio->stopHardware(log);
  if (!sessionState_.finishFinalize()) return false;
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
    while (sessionState_.captureActive()) {
      const AudioCaptureCycleResult cycle = service_.captureOnce(millis());
      if (cycle == AudioCaptureCycleResult::frameDropped ||
          cycle == AudioCaptureCycleResult::sourceTimeout ||
          cycle == AudioCaptureCycleResult::sourceEarlyZero ||
          cycle == AudioCaptureCycleResult::sourceOverrun ||
          cycle == AudioCaptureCycleResult::sourceFailure) {
        incomplete_.store(true, std::memory_order_release);
      }
      if (cycle == AudioCaptureCycleResult::sourceFailure ||
          cycle == AudioCaptureCycleResult::sourceEarlyZero) taskYIELD();
      if (cycle == AudioCaptureCycleResult::sourceWarmingUp) {
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    }
    if (sessionState_.taskStopped()) {
      xSemaphoreGive(stopped_);
    }
  }
}

}  // namespace pokepod
