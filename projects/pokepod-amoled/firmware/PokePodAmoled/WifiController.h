#pragma once

#include <Arduino.h>
#include <vector>

#include "DeviceConfig.h"
#include "NetworkTimeSyncState.h"
#include "WifiPolicy.h"

namespace pokepod {

class RuntimeDiagnostics;

class WifiController {
 public:
  bool begin(DeviceConfig &config, Print &log);
  void bindRuntimeDiagnostics(RuntimeDiagnostics &diagnostics) {
    runtimeDiagnostics_ = &diagnostics;
  }
  void loop(uint32_t nowMs, bool recording, bool pendingWork, bool charging,
            bool provisioning, bool wirelessSync,
            bool audioCaptureExclusive = false);
  void configurationChanged();
  void requestConnection();
  void pauseForAudioCapture(Print &log);
  void quiesceForProvisioning(Print &log);
  void prepareForSleep();

  WifiPhase phase() const { return decision_.phase; }
  bool connected() const { return connected_; }
  bool shouldProcessQueue() const { return decision_.processQueue; }
  int32_t rssi() const;
  const char *phaseName() const;
  bool timeReady() const;
  bool networkTimeSynchronized() const;
  uint32_t networkTimeSyncRevision() const {
    return timeSyncState_.revision();
  }
  uint32_t connectionGeneration() const {
    return timeSyncState_.connectionGeneration();
  }
  void handleTimeSyncNotification();
  uint16_t lastDisconnectReason() const;
  bool radioOn() const { return radioOn_; }
  bool powerSaveEnabled() const { return powerSaveEnabled_; }
  int32_t powerSaveError() const { return powerSaveError_; }

 private:
  void startConnection(uint32_t nowMs);
  void startScan(uint32_t nowMs);
  void pollScan(uint32_t nowMs);
  void buildCandidateOrder(int16_t scanCount);
  void connectCandidate(uint32_t nowMs);
  void stopRadio();
  void noteFailure(uint32_t nowMs);
  void setPowerSave(bool enabled);

  DeviceConfig *config_ = nullptr;
  Print *log_ = nullptr;
  WifiDecision decision_;
  bool radioOn_ = false;
  bool connected_ = false;
  bool connectionFailed_ = false;
  bool exhausted_ = false;
  bool previousDemand_ = false;
  bool previousCharging_ = false;
  bool manualWakeRequested_ = false;
  bool ntpStarted_ = false;
  bool scanning_ = false;
  bool successfulNetworkNoted_ = false;
  uint8_t failedAttempts_ = 0;
  uint32_t connectionStartedMs_ = 0;
  uint32_t scanStartedMs_ = 0;
  uint32_t retryAtMs_ = 0;
  uint32_t lastMruPersistAttemptMs_ = 0;
  std::vector<uint8_t> candidateOrder_;
  size_t candidatePosition_ = 0;
  bool powerSaveConfigured_ = false;
  bool powerSaveEnabled_ = false;
  int32_t powerSaveError_ = 0;
  NetworkTimeSyncState timeSyncState_;
  RuntimeDiagnostics *runtimeDiagnostics_ = nullptr;
};

}  // namespace pokepod
