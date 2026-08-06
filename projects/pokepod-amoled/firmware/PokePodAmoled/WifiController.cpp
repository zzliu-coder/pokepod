#include "WifiController.h"

#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

namespace pokepod {
namespace {
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
}

bool WifiController::begin(DeviceConfig &config, Print &log) {
  config_ = &config;
  log_ = &log;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  return true;
}

void WifiController::loop(uint32_t nowMs, bool recording, bool pendingWork,
                          bool charging, bool provisioning) {
  if (config_ == nullptr) return;
  const bool demand = recording || pendingWork || charging;
  if ((!previousDemand_ && demand) || (!previousCharging_ && charging)) {
    exhausted_ = false;
    failedAttempts_ = 0;
    retryAtMs_ = 0;
    connectionFailed_ = false;
  }
  previousDemand_ = demand;
  previousCharging_ = charging;

  connected_ = WiFi.status() == WL_CONNECTED;
  WifiInputs inputs;
  inputs.configured = config_->hasWifi();
  inputs.manuallyDisabled = !config_->settings().wifiEnabled;
  inputs.charging = charging;
  inputs.recording = recording;
  inputs.pendingWork = pendingWork;
  inputs.connected = connected_;
  inputs.provisioning = provisioning;
  inputs.connectionFailed = connectionFailed_ || exhausted_;
  decision_ = nextWifiDecision(decision_, inputs, nowMs);

  if (provisioning) {
    radioOn_ = true;
    return;
  }
  if (!decision_.radioOn || exhausted_) {
    stopRadio();
    return;
  }
  if (connected_) {
    connectionFailed_ = false;
    failedAttempts_ = 0;
    if (!ntpStarted_) {
      configTime(0, 0, "ntp.tencent.com", "pool.ntp.org");
      ntpStarted_ = true;
      log_->println("{\"event\":\"wifi_online\",\"ntp\":\"started\"}");
    }
    return;
  }
  if (radioOn_ && connectionStartedMs_ != 0 &&
      static_cast<uint32_t>(nowMs - connectionStartedMs_) >= kWifiConnectTimeoutMs) {
    noteFailure(nowMs);
  }
  if (!radioOn_ && (retryAtMs_ == 0 ||
      static_cast<int32_t>(nowMs - retryAtMs_) >= 0)) {
    startConnection(nowMs);
  }
}

void WifiController::configurationChanged() {
  stopRadio();
  decision_ = WifiDecision();
  failedAttempts_ = 0;
  retryAtMs_ = 0;
  exhausted_ = false;
  connectionFailed_ = false;
  ntpStarted_ = false;
}

void WifiController::startConnection(uint32_t nowMs) {
  if (config_ == nullptr || !config_->hasWifi()) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(config_->settings().wifiSsid.c_str(),
             config_->settings().wifiPassword.c_str());
  radioOn_ = true;
  connectionStartedMs_ = nowMs == 0 ? 1 : nowMs;
  connectionFailed_ = false;
  log_->printf("{\"event\":\"wifi_connecting\",\"attempt\":%u}\n",
               static_cast<unsigned>(failedAttempts_ + 1));
}

void WifiController::stopRadio() {
  if (!radioOn_ && WiFi.getMode() == WIFI_OFF) return;
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);
  radioOn_ = false;
  connected_ = false;
  connectionStartedMs_ = 0;
  ntpStarted_ = false;
}

void WifiController::noteFailure(uint32_t nowMs) {
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);
  radioOn_ = false;
  connected_ = false;
  connectionStartedMs_ = 0;
  connectionFailed_ = true;
  const uint32_t retryDelay = wifiRetryDelayMs(failedAttempts_);
  ++failedAttempts_;
  if (retryDelay == 0) {
    exhausted_ = true;
    retryAtMs_ = 0;
    log_->println("{\"event\":\"wifi_waiting_for_wake\",\"queue_preserved\":true}");
  } else {
    retryAtMs_ = nowMs + retryDelay;
    log_->printf("{\"event\":\"wifi_retry\",\"after_ms\":%lu}\n",
                 static_cast<unsigned long>(retryDelay));
  }
}

int32_t WifiController::rssi() const {
  return connected_ ? WiFi.RSSI() : 0;
}

const char *WifiController::phaseName() const {
  switch (decision_.phase) {
    case WifiPhase::disabled: return "disabled";
    case WifiPhase::off: return "off";
    case WifiPhase::connecting: return "connecting";
    case WifiPhase::online: return "online";
    case WifiPhase::grace: return "grace";
    case WifiPhase::provisioning: return "provisioning";
    case WifiPhase::error: return "error";
  }
  return "unknown";
}

bool WifiController::timeReady() const {
  return time(nullptr) >= 1704067200;
}

bool WifiController::networkTimeSynchronized() const {
  return ntpStarted_ && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
}

}  // namespace pokepod
