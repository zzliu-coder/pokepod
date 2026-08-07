#pragma once

#include <Arduino.h>
#include <FS.h>
#include <atomic>

#include "CapsuleLibrary.h"
#include "DeviceConfig.h"
#include "TencentAsr.h"

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
  bool working() const { return working_.load(); }
  bool waitingForWake() const { return waitingForWake_; }
  uint32_t lastHashElapsedMs() const { return lastHashElapsedMs_; }
  uint32_t lastConnectElapsedMs() const { return lastConnectElapsedMs_; }
  uint32_t lastUploadElapsedMs() const { return lastUploadElapsedMs_; }
  uint32_t lastTotalElapsedMs() const { return lastTotalElapsedMs_; }

 private:
  static void taskEntry(void *context);
  void runAttempt();
  void finishAttempt(uint32_t nowMs);

  fs::FS *fs_ = nullptr;
  CapsuleLibrary *library_ = nullptr;
  DeviceConfig *config_ = nullptr;
  Print *log_ = nullptr;
  TencentAsr asr_;
  String retryingCapsuleId_;
  std::atomic<bool> working_{false};
  std::atomic<bool> resultReady_{false};
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
};

}  // namespace pokepod
