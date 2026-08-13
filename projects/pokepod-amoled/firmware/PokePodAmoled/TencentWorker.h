#pragma once

#include <Arduino.h>
#include <FS.h>
#include <atomic>

#include "CapsuleLibrary.h"
#include "DeviceConfig.h"
#include "TencentAsr.h"
#include "TencentJobRuntime.h"

namespace pokepod {

class TencentWorker {
 public:
  bool begin(fs::FS &fs, CapsuleLibrary &library, DeviceConfig &config,
             Print &log);
  void wake() {
    waitingForWake_ = false;
    transientFailures_ = 0;
    nextAttemptMs_ = 0;
  }
  void loop(uint32_t nowMs, bool networkReady, bool timeReady,
            bool foregroundBusy, bool charging);
  bool working() const {
    return runtime_.working();
  }
  TencentJobState state() const {
    return runtime_.state();
  }
  TencentAsrStage stage() const {
    return static_cast<TencentAsrStage>(
        stage_.load(std::memory_order_acquire));
  }
  const char *stateName() const { return tencentJobStateName(state()); }
  uint32_t generation() const {
    return runtime_.generation();
  }
  bool cancel(TencentCancelReason reason);
  bool quiesce(uint32_t nowMs, uint32_t timeoutMs,
               TencentCancelReason reason);
  bool waitingForWake() const { return waitingForWake_; }
  uint32_t lastHashElapsedMs() const { return lastHashElapsedMs_; }
  uint32_t lastConnectElapsedMs() const { return lastConnectElapsedMs_; }
  uint32_t lastUploadElapsedMs() const { return lastUploadElapsedMs_; }
  uint32_t lastTotalElapsedMs() const { return lastTotalElapsedMs_; }
  const String &lastCode() const { return lastCode_; }
  const String &lastNetworkErrorDetail() const { return lastNetworkErrorDetail_; }
  int32_t lastNetworkError() const { return lastNetworkError_; }
  uint32_t lastInternalHeapFreeBeforeTls() const {
    return lastInternalHeapFreeBeforeTls_;
  }
  uint32_t lastInternalHeapLargestBeforeTls() const {
    return lastInternalHeapLargestBeforeTls_;
  }
  uint32_t lastPsramFreeBeforeTls() const { return lastPsramFreeBeforeTls_; }

 private:
  static constexpr uint32_t kAttemptWatchdogMs = 180000;
  static void taskEntry(void *context);
  void taskLoop();
  void runAttempt(uint32_t generation);
  void finishAttempt(uint32_t nowMs);
  void clearTaskSecrets();
  void logState(const char *event, TencentJobState state,
                uint32_t generation) const;

  fs::FS *fs_ = nullptr;
  CapsuleLibrary *library_ = nullptr;
  DeviceConfig *config_ = nullptr;
  Print *log_ = nullptr;
  TencentAsr asr_;
  TencentJobRuntime runtime_;
  TaskHandle_t taskHandle_ = nullptr;
  String retryingCapsuleId_;
  std::atomic<uint8_t> stage_{
      static_cast<uint8_t>(TencentAsrStage::idle)};
  std::atomic<bool> resultReady_{false};
  std::atomic<uint32_t> resultGeneration_{0};
  TencentAsrResult taskResult_;
  DeviceSettings taskSettings_;
  String taskCapsuleId_;
  String taskAudioPath_;
  bool waitingForWake_ = false;
  bool previousCharging_ = false;
  uint8_t transientFailures_ = 0;
  uint32_t nextAttemptMs_ = 0;
  uint32_t lastHashElapsedMs_ = 0;
  uint32_t lastConnectElapsedMs_ = 0;
  uint32_t lastUploadElapsedMs_ = 0;
  uint32_t lastTotalElapsedMs_ = 0;
  String lastCode_;
  String lastNetworkErrorDetail_;
  int32_t lastNetworkError_ = 0;
  uint32_t lastInternalHeapFreeBeforeTls_ = 0;
  uint32_t lastInternalHeapLargestBeforeTls_ = 0;
  uint32_t lastPsramFreeBeforeTls_ = 0;
};

}  // namespace pokepod
