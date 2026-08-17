#include "TencentWorker.h"

#include <esp_heap_caps.h>

#include "CapsulePolicy.h"
#include "DeviceSecretWipe.h"
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
    runtime_.workerStartResult(false);
    log.println("{\"event\":\"asr_worker_start\",\"ok\":false}");
    return false;
  }
  runtime_.workerStartResult(true);
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

  if (runtime_.checkWatchdog(nowMs)) {
    logState("watchdog", TencentJobState::watchdog, runtime_.generation());
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
    runtime_.failBeforeRequest();
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
  const uint32_t generation = runtime_.request(nowMs + kAttemptWatchdogMs);
  if (generation == 0) {
    library_->markRetryable(id, "transcription", "转写任务正忙");
    clearTaskSecrets();
    return;
  }
  logState("queued", TencentJobState::queued, generation);
  xTaskNotifyGive(taskHandle_);
}

bool TencentWorker::cancel(TencentCancelReason reason) {
  if (reason == TencentCancelReason::none) reason = TencentCancelReason::user;
  if (!runtime_.cancel(reason)) return false;
  const TencentJobState next = reason == TencentCancelReason::watchdog
      ? TencentJobState::watchdog : TencentJobState::cancelling;
  logState(reason == TencentCancelReason::watchdog ? "watchdog" : "cancel",
           next, runtime_.generation());
  return true;
}

bool TencentWorker::quiesce(uint32_t nowMs, uint32_t timeoutMs,
                            TencentCancelReason reason) {
  TencentQuiesceStatus status = beginQuiesce(nowMs, timeoutMs, reason);
  while (status == TencentQuiesceStatus::waiting) {
    // The legacy synchronous shutdown API retains its historical durable
    // settlement semantics.  Link reboot uses pollQuiesce directly and never
    // enters this path.
    if (resultReady_.load(std::memory_order_acquire)) finishAttempt(millis());
    status = pollQuiesce(millis());
    if (status == TencentQuiesceStatus::waiting) delay(5);
  }
  return status == TencentQuiesceStatus::complete;
}

TencentQuiesceStatus TencentWorker::beginQuiesce(
    uint32_t nowMs, uint32_t timeoutMs, TencentCancelReason reason) {
  return runtime_.beginQuiesce(nowMs, timeoutMs, reason);
}

TencentQuiesceStatus TencentWorker::pollQuiesce(uint32_t nowMs) {
  return runtime_.pollQuiesce(nowMs);
}

bool TencentWorker::abandonResultForReboot() {
  if (!resultReady_.exchange(false, std::memory_order_acq_rel)) return false;
  const uint32_t generation =
      resultGeneration_.load(std::memory_order_acquire);
  clearTaskSecrets();
  if (generation == 0 || !runtime_.matchesGeneration(generation)) return false;
  const bool abandoned = runtime_.abandonForReboot(generation);
  if (abandoned && log_ != nullptr) {
    log_->printf(
        "{\"event\":\"asr_result_abandoned_for_reboot\",\"generation\":%lu}\n",
        static_cast<unsigned long>(generation));
  }
  return abandoned;
}

void TencentWorker::taskEntry(void *context) {
  static_cast<TencentWorker *>(context)->taskLoop();
}

void TencentWorker::taskLoop() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const uint32_t generation = runtime_.generation();
    if (generation == 0) continue;
    runAttempt(generation);
  }
}

void TencentWorker::runAttempt(uint32_t generation) {
  TencentAsrControl control = {
      runtime_.cancelToken(), generation, &stage_,
  };
  const bool started = runtime_.start(generation);
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
  // The network task has made every copy it needs.  Erase the shared
  // task-local credentials before publishing any terminal result, including
  // cancellation and stale-generation results.
  clearTaskSecrets();
  resultGeneration_.store(generation, std::memory_order_relaxed);
  resultReady_.store(true, std::memory_order_release);
}

void TencentWorker::finishAttempt(uint32_t nowMs) {
  if (!resultReady_.exchange(false, std::memory_order_acq_rel)) return;
  const uint32_t generation =
      resultGeneration_.load(std::memory_order_acquire);
  if (generation == 0 ||
      !runtime_.matchesGeneration(generation)) {
    clearTaskSecrets();
    if (log_ != nullptr) {
      log_->printf(
          "{\"event\":\"asr_stale_result\",\"generation\":%lu}\n",
          static_cast<unsigned long>(generation));
    }
    return;
  }

  const String id = taskCapsuleId_;
  const TencentAsrResult result = taskResult_;
  clearTaskSecrets();
  const TencentCancelReason cancelReason = runtime_.cancelReason();
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
  if (!runtime_.networkFinished(generation, result.ok, result.transient)) {
    logState("stale_result", state(), generation);
    return;
  }

  if (state() == TencentJobState::cancelled ||
      (state() == TencentJobState::retryable &&
       cancelReason == TencentCancelReason::watchdog)) {
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
    } else {
      waitingForWake_ = true;
      nextAttemptMs_ = 0;
    }
    logState(watchdog ? "watchdog_finished" : "cancelled", state(),
             generation);
    return;
  }

  if (state() == TencentJobState::committing) {
    logState("committing", TencentJobState::committing, generation);
    const bool committed = library_->commitRawText(id, result.text);
    if (!runtime_.commitFinished(generation, committed)) {
      logState("commit_stale", state(), generation);
      return;
    }
    if (committed) {
      retryingCapsuleId_ = "";
      transientFailures_ = 0;
      nextAttemptMs_ = 0;
      logState("succeeded", TencentJobState::succeeded, generation);
    } else {
      library_->markFailure(id, "storage",
                            "转写成功但 raw.txt 提交失败");
      lastCode_ = "COMMIT_FAILED";
      logState("commit_failed", TencentJobState::failed, generation);
    }
    return;
  }

  if (state() == TencentJobState::failed) {
    library_->markFailure(id, "transcription",
                          result.code + ": " + result.message);
    retryingCapsuleId_ = "";
    transientFailures_ = 0;
    nextAttemptMs_ = 0;
    logState("failed", TencentJobState::failed, generation);
    return;
  }

  library_->markRetryable(id, "transcription",
                          result.code + ": " + result.message);
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

void TencentWorker::clearTaskSecrets() {
  secureWipeSecrets(taskSettings_);
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
