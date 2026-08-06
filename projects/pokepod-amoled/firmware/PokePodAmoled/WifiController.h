#pragma once

#include <Arduino.h>

#include "DeviceConfig.h"
#include "WifiPolicy.h"

namespace pokepod {

class WifiController {
 public:
  bool begin(DeviceConfig &config, Print &log);
  void loop(uint32_t nowMs, bool recording, bool pendingWork, bool charging,
            bool provisioning);
  void configurationChanged();

  WifiPhase phase() const { return decision_.phase; }
  bool connected() const { return connected_; }
  bool shouldProcessQueue() const { return decision_.processQueue; }
  int32_t rssi() const;
  const char *phaseName() const;
  bool timeReady() const;
  bool networkTimeSynchronized() const;

 private:
  void startConnection(uint32_t nowMs);
  void stopRadio();
  void noteFailure(uint32_t nowMs);

  DeviceConfig *config_ = nullptr;
  Print *log_ = nullptr;
  WifiDecision decision_;
  bool radioOn_ = false;
  bool connected_ = false;
  bool connectionFailed_ = false;
  bool exhausted_ = false;
  bool previousDemand_ = false;
  bool previousCharging_ = false;
  bool ntpStarted_ = false;
  uint8_t failedAttempts_ = 0;
  uint32_t connectionStartedMs_ = 0;
  uint32_t retryAtMs_ = 0;
};

}  // namespace pokepod
