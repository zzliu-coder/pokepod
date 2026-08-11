#include "TencentWorker.h"

#include <esp_heap_caps.h>

#include "CapsulePolicy.h"
#include "WifiPolicy.h"

namespace pokepod {

bool TencentWorker::begin(fs::FS &fs, CapsuleLibrary &library,
                          DeviceConfig &config, Print &log) {
  fs_ = &fs;
  library_ = &library;
  config_ = &config;
  log_ = &log;
  if (taskHandle_ != nullptr) return true;
  if (xTaskCreatePinnedToCoreWithCaps(
          taskEntry, "pokepod-asr", 16384, this, 1, &taskHandle_, 0,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    taskHandle_ = nullptr;
    setState(TencentJobState::failed);
    log.println("{\"event\":\"asr_worker_start\",\"ok\":false}");
    return false;
  }
  setState(TencentJobState::idle);
  log.println("{\"event\":\"asr_worker_start\",\"ok\":true,\"persistent\":true}");
  return true;
}

void TencentWorker::loop(uint32_t nowMs, bool networkReady, bool timeReady,
                         bool foregroundBusy, bool charging) {
  if (!previousCharging_ && charging) {
    waitingForWake_ = false;
    transientFailures_ = 0;
    nextAttemptMs_ = 0;
  }
  previousCharging_ = charging;

  if (resultReady_.load(std::memory_order_acquire)) finishAttempt(nowMs);

  const TencentJobState current = state();
  if ((current == TencentJobState::queued ||
       current == TencentJobState::working) &&
      tencentDeadlineReached(nowMs, attemptDeadlineMs_)) {
    cancel(TencentCancelReason::watchdog);
  }

  if (fs_ == nullptr || library_ == nullptr || config_ == nullptr ||
      taskHandle_ == nullptr || working() || foregroundBusy || !networkReady ||
      !timeReady || !config_->hasTencent() || waitingForWake_ ||
      (nextAttemptMs_ != 0 &&
       static_cast<int32_t>(nowMs - nextAttemptMs_) < 0)) {
    return;
  }
  const CapsuleSummary *queued = library_->nextQueued();
  if (queued == nullptr) return;
  const String id = queued->id;
  const String audioFile = queued->audioFile;
  const String directory = queued->directory;
  if (!safeCapsuleFileName(audioFile.c_str()) || audioFile != "audio.wav") {
    library_->markFailure(id, "audio", "不支持的胶囊音频路径");
    setState(TencentJobState::failed);
    return;
  }
  if (retryingCapsuleId_ != id) {
    retryingCapsuleId_ = id;
    transientFailures_ = 0;
    nextAttemptMs_ = 0;
  }
  if (!library_->markTranscribing(id)) return;

  taskCapsuleId_ = id;
  taskAudioPath_ = directory + "/" + audioFile;
  taskSettings_ = config_->settings();
  taskResult_ = TencentAsrResult();
  resultReady_.store(false, std::memory_order_relaxed);
  resultGeneration_.store(0, std::memory_order_relaxed);
  stage_.store(static_cast<uint8_t>(TencentAsrStage::idle),
               std::memory_order_relaxed);
  cancelReason_.store(static_cast<uint8_t>(TencentCancelReason::none),
                      std::memory_order_relaxed);
  const uint32_t generation = cancelToken_.begin();
  activeGeneration_.store(generation, std::memory_order_release);
  attemptDeadlineMs_ = nowMs + kAttemptWatchdogMs;
  setState(TencentJobState::queued);
  logState("queued", TencentJobState::queued, generation);
  xTaskNotifyGive(taskHandle_);
}

bool TencentWorker::cancel(TencentCancelReason reason) {
  const TencentJobState current = state();
  if (current == TencentJobState::cancelling ||
      current == TencentJobState::watchdog) {
    return true;
  }
  if (current != TencentJobState::queued &&
      current != TencentJobState::working) {
    return false;
  }
  const uint32_t generation = activeGeneration_.load(std::memory_order_acquire);
  if (!cancelToken_.cancel(generation)) return false;
  if (reason == TencentCancelReason::none) reason = TencentCancelReason::user;
  cancelReason_.store(static_cast<uint8_t>(reason), std::memory_order_release);
  const TencentJobState next = reason == TencentCancelReason::watchdog
      ? TencentJobState::watchdog : TencentJobState::cancelling;
  setState(next);
  logState(reason == TencentCancelReason::watchdog ? "watchdog" : "cancel",
           next, generation);
  return true;
}

bool TencentWorker::quiesce(uint32_t nowMs, uint32_t timeoutMs,
                            TencentCancelReason reason) {
  if (!working()) return true;
  cancel(reason);
  const uint32_t deadlineMs = nowMs + timeoutMs;
  while (working() &&
         static_cast<int32_t>(millis() - deadlineMs) < 0) {
    if (resultReady_.load(std::memory_order_acquire)) finishAttempt(millis());
    if (working()) delay(5);
  }
  if (resultReady_.load(std::memory_order_acquire)) finishAttempt(millis());
  return !working();
}

void TencentWorker::taskEntry(void *context) {
  static_cast<TencentWorker *>(context)->taskLoop();
}

void TencentWorker::taskLoop() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const uint32_t generation =
        activeGeneration_.load(std::memory_order_acquire);
    if (generation == 0) continue;
    runAttempt(generation);
  }
}

void TencentWorker::runAttempt(uint32_t generation) {
  TencentAsrControl control = {
      &cancelToken_, generation, &stage_,
  };
  uint8_t expected = static_cast<uint8_t>(TencentJobState::queued);
  const bool started = !control.cancelled() && state_.compare_exchange_strong(
      expected, static_cast<uint8_t>(TencentJobState::working),
      std::memory_order_acq_rel);
  if (started) {
    logState("started", TencentJobState::working, generation);
  }
  taskResult_ = TencentAsrResult();
  if (!started || control.cancelled()) {
    taskResult_.code = "CANCELLED";
    taskResult_.message = "转写已取消";
    control.setStage(TencentAsrStage::cancelled);
  } else {
    asr_.transcribe(*fs_, taskAudioPath_, taskSettings_, taskResult_, *log_,
                    &control);
  }
  resultGeneration_.store(generation, std::memory_order_relaxed);
  resultReady_.store(true, std::memory_order_release);
}

void TencentWorker::finishAttempt(uint32_t nowMs) {
  if (!resultReady_.exchange(false, std::memory_order_acq_rel)) return;
  const uint32_t generation =
      resultGeneration_.load(std::memory_order_acquire);
  if (generation == 0 ||
      generation != activeGeneration_.load(std::memory_order_acquire)) {
    if (log_ != nullptr) {
      log_->printf(
          "{\"event\":\"asr_stale_result\",\"generation\":%lu}\n",
          static_cast<unsigned long>(generation));
    }
    return;
  }

  const String id = taskCapsuleId_;
  const TencentAsrResult result = taskResult_;
  const TencentCancelReason cancelReason = static_cast<TencentCancelReason>(
      cancelReason_.load(std::memory_order_acquire));
  lastHashElapsedMs_ = result.hashElapsedMs;
  lastConnectElapsedMs_ = result.connectElapsedMs;
  lastUploadElapsedMs_ = result.uploadElapsedMs;
  lastTotalElapsedMs_ = result.totalElapsedMs;
  lastCode_ = result.code;
  lastNetworkError_ = result.networkError;
  lastNetworkErrorDetail_ = result.networkErrorDetail;
  lastInternalHeapFreeBeforeTls_ = result.internalHeapFreeBeforeTls;
  lastInternalHeapLargestBeforeTls_ = result.internalHeapLargestBeforeTls;
  lastPsramFreeBeforeTls_ = result.psramFreeBeforeTls;
  attemptDeadlineMs_ = 0;

  if (cancelReason != TencentCancelReason::none || result.code == "CANCELLED") {
    const bool watchdog = cancelReason == TencentCancelReason::watchdog;
    const String detail = watchdog ? "转写超时" : "转写已取消";
    library_->markRetryable(id, "transcription", detail);
    if (watchdog) {
      const uint32_t delayMs = wifiRetryDelayMs(transientFailures_);
      ++transientFailures_;
      if (delayMs == 0) {
        waitingForWake_ = true;
        nextAttemptMs_ = 0;
      } else {
        nextAttemptMs_ = nowMs + delayMs;
      }
      setState(TencentJobState::retryable);
    } else {
      waitingForWake_ = true;
      nextAttemptMs_ = 0;
      setState(TencentJobState::cancelled);
    }
    logState(watchdog ? "watchdog_finished" : "cancelled", state(),
             generation);
    return;
  }

  if (result.ok) {
    setState(TencentJobState::committing);
    logState("committing", TencentJobState::committing, generation);
    if (library_->commitRawText(id, result.text)) {
      setState(TencentJobState::succeeded);
      retryingCapsuleId_ = "";
      transientFailures_ = 0;
      nextAttemptMs_ = 0;
      logState("succeeded", TencentJobState::succeeded, generation);
    } else {
      library_->markFailure(id, "storage",
                            "转写成功但 raw.txt 提交失败");
      setState(TencentJobState::failed);
      lastCode_ = "COMMIT_FAILED";
      logState("commit_failed", TencentJobState::failed, generation);
    }
    return;
  }

  if (!result.transient) {
    library_->markFailure(id, "transcription",
                          result.code + ": " + result.message);
    retryingCapsuleId_ = "";
    transientFailures_ = 0;
    nextAttemptMs_ = 0;
    setState(TencentJobState::failed);
    logState("failed", TencentJobState::failed, generation);
    return;
  }

  library_->markRetryable(id, "transcription",
                          result.code + ": " + result.message);
  setState(TencentJobState::retryable);
  const uint32_t delayMs = wifiRetryDelayMs(transientFailures_);
  ++transientFailures_;
  if (delayMs == 0) {
    waitingForWake_ = true;
    nextAttemptMs_ = 0;
    log_->printf(
        "{\"event\":\"asr_waiting_for_wake\",\"capsule_id\":\"%s\"}\n",
        id.c_str());
  } else {
    nextAttemptMs_ = nowMs + delayMs;
    log_->printf(
        "{\"event\":\"asr_retry\",\"capsule_id\":\"%s\",\"after_ms\":%lu}\n",
        id.c_str(), static_cast<unsigned long>(delayMs));
  }
  logState("retryable", TencentJobState::retryable, generation);
}

void TencentWorker::logState(const char *event, TencentJobState state,
                             uint32_t generation) const {
  if (log_ == nullptr) return;
  log_->printf(
      "{\"event\":\"asr_job\",\"transition\":\"%s\",\"state\":\"%s\",\"generation\":%lu}\n",
      event == nullptr ? "" : event, tencentJobStateName(state),
      static_cast<unsigned long>(generation));
}

}  // namespace pokepod
