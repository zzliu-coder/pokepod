#include "TencentWorker.h"

#include "CapsulePolicy.h"
#include "WifiPolicy.h"

namespace pokepod {

bool TencentWorker::begin(fs::FS &fs, CapsuleLibrary &library,
                          DeviceConfig &config, Print &log) {
  fs_ = &fs;
  library_ = &library;
  config_ = &config;
  log_ = &log;
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
  if (fs_ == nullptr || library_ == nullptr || config_ == nullptr ||
      working_.load() || foregroundBusy || !networkReady || !timeReady ||
      !config_->hasTencent() || waitingForWake_ ||
      (nextAttemptMs_ != 0 && static_cast<int32_t>(nowMs - nextAttemptMs_) < 0)) {
    return;
  }
  const CapsuleSummary *queued = library_->nextQueued();
  if (queued == nullptr) return;
  const String id = queued->id;
  const String audioFile = queued->audioFile;
  const String directory = queued->directory;
  if (!safeCapsuleFileName(audioFile.c_str()) || audioFile != "audio.wav") {
    library_->markFailure(id, "audio", "不支持的胶囊音频路径");
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
  working_.store(true, std::memory_order_release);
  if (xTaskCreatePinnedToCore(taskEntry, "pokepod-asr", 16384, this, 1,
                             nullptr, 0) != pdPASS) {
    working_.store(false, std::memory_order_release);
    library_->markRetryable(id, "transcription", "无法启动转写后台任务");
    nextAttemptMs_ = nowMs + wifiRetryDelayMs(0);
  }
}

void TencentWorker::taskEntry(void *context) {
  static_cast<TencentWorker *>(context)->runAttempt();
  vTaskDelete(nullptr);
}

void TencentWorker::runAttempt() {
  asr_.transcribe(*fs_, taskAudioPath_, taskSettings_, taskResult_, *log_);
  resultReady_.store(true, std::memory_order_release);
}

void TencentWorker::finishAttempt(uint32_t nowMs) {
  if (!resultReady_.exchange(false, std::memory_order_acq_rel)) return;
  working_.store(false, std::memory_order_release);
  const String id = taskCapsuleId_;
  const TencentAsrResult result = taskResult_;
  lastHashElapsedMs_ = result.hashElapsedMs;
  lastConnectElapsedMs_ = result.connectElapsedMs;
  lastUploadElapsedMs_ = result.uploadElapsedMs;
  lastTotalElapsedMs_ = result.totalElapsedMs;
  if (result.ok) {
    if (!library_->commitRawText(id, result.text)) {
      library_->markFailure(id, "storage", "转写成功但 raw.txt 提交失败");
    }
    retryingCapsuleId_ = "";
    transientFailures_ = 0;
    nextAttemptMs_ = 0;
    return;
  }
  if (!result.transient) {
    library_->markFailure(id, "transcription", result.code + ": " + result.message);
    retryingCapsuleId_ = "";
    transientFailures_ = 0;
    nextAttemptMs_ = 0;
    return;
  }

  library_->markRetryable(id, "transcription", result.code + ": " + result.message);
  const uint32_t delayMs = wifiRetryDelayMs(transientFailures_);
  ++transientFailures_;
  if (delayMs == 0) {
    waitingForWake_ = true;
    nextAttemptMs_ = 0;
    log_->printf("{\"event\":\"asr_waiting_for_wake\",\"capsule_id\":\"%s\"}\n",
                 id.c_str());
  } else {
    nextAttemptMs_ = nowMs + delayMs;
    log_->printf("{\"event\":\"asr_retry\",\"capsule_id\":\"%s\",\"after_ms\":%lu}\n",
                 id.c_str(), static_cast<unsigned long>(delayMs));
  }
}

}  // namespace pokepod
