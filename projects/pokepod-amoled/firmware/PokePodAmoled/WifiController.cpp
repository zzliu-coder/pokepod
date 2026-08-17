#include "WifiController.h"

#include <WiFi.h>
#include <algorithm>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <time.h>

#include "RememberedWifiPolicy.h"
#include "WifiDisconnectDiagnostics.h"

namespace pokepod {
namespace {
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kWifiScanTimeoutMs = 8000;
constexpr uint32_t kWifiCandidateDelayMs = 500;
constexpr uint32_t kWifiMruPersistRetryMs = 5000;
WifiController *activeTimeSyncController = nullptr;

void timeSyncNotification(struct timeval *) {
  if (activeTimeSyncController != nullptr) {
    activeTimeSyncController->handleTimeSyncNotification();
  }
}
}

bool WifiController::begin(DeviceConfig &config, Print &log) {
  config_ = &config;
  log_ = &log;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  beginWifiDisconnectDiagnostics();
  activeTimeSyncController = this;
  sntp_set_time_sync_notification_cb(timeSyncNotification);
  WiFi.mode(WIFI_OFF);
  return true;
}

void WifiController::loop(uint32_t nowMs, bool recording, bool pendingWork,
                          bool charging, bool provisioning,
                          bool wirelessSync, bool audioCaptureExclusive) {
  if (config_ == nullptr) return;
  if (audioCaptureExclusive && !provisioning) {
    stopRadio();
    previousDemand_ = false;
    return;
  }
  connected_ = WiFi.status() == WL_CONNECTED;
  timeSyncState_.noteConnected(connected_);
  if (connected_) manualWakeRequested_ = false;
  const bool demand = recording || pendingWork || charging || wirelessSync ||
      manualWakeRequested_;
  if ((!previousDemand_ && demand) || (!previousCharging_ && charging)) {
    exhausted_ = false;
    failedAttempts_ = 0;
    retryAtMs_ = 0;
    connectionFailed_ = false;
  }
  previousDemand_ = demand;
  previousCharging_ = charging;

  WifiInputs inputs;
  inputs.configured = config_->hasWifi();
  inputs.manuallyDisabled = !config_->settings().wifiEnabled;
  inputs.charging = charging;
  inputs.recording = recording;
  inputs.pendingWork = pendingWork;
  inputs.wirelessSync = wirelessSync;
  inputs.manualWake = manualWakeRequested_;
  inputs.connected = connected_;
  inputs.provisioning = provisioning;
  inputs.connectionFailed = connectionFailed_ || exhausted_;
  decision_ = nextWifiDecision(decision_, inputs, nowMs);

  if (provisioning) {
    radioOn_ = WiFi.getMode() != WIFI_OFF;
    return;
  }
  if (!decision_.radioOn || exhausted_) {
    stopRadio();
    return;
  }
  if (connected_) {
    setPowerSave(true);
    connectionFailed_ = false;
    failedAttempts_ = 0;
    if (!successfulNetworkNoted_ &&
        (lastMruPersistAttemptMs_ == 0 ||
         static_cast<uint32_t>(nowMs - lastMruPersistAttemptMs_) >=
             kWifiMruPersistRetryMs)) {
      lastMruPersistAttemptMs_ = nowMs == 0 ? 1 : nowMs;
      if (config_->markWifiSuccessful(WiFi.SSID(), *log_)) {
        successfulNetworkNoted_ = true;
        candidateOrder_.clear();
        candidatePosition_ = 0;
      }
    }
    if (!ntpStarted_) {
      configTime(0, 0, "ntp.tencent.com", "pool.ntp.org");
      ntpStarted_ = true;
      log_->println("{\"event\":\"wifi_online\",\"ntp\":\"started\"}");
    }
    return;
  }
  if (scanning_) {
    pollScan(nowMs);
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

void WifiController::pauseForAudioCapture(Print &log) {
  const bool wasActive = radioOn_ || WiFi.getMode() != WIFI_OFF;
  stopRadio();
  previousDemand_ = false;
  log.printf(
      "{\"event\":\"wifi_audio_capture_pause\",\"was_active\":%s}\n",
      wasActive ? "true" : "false");
}

void WifiController::configurationChanged() {
  stopRadio();
  decision_ = WifiDecision();
  failedAttempts_ = 0;
  retryAtMs_ = 0;
  exhausted_ = false;
  connectionFailed_ = false;
  ntpStarted_ = false;
  scanning_ = false;
  successfulNetworkNoted_ = false;
  lastMruPersistAttemptMs_ = 0;
  candidateOrder_.clear();
  candidatePosition_ = 0;
  manualWakeRequested_ = false;
  WiFi.scanDelete();
}

void WifiController::requestConnection() {
  configurationChanged();
  manualWakeRequested_ = true;
  if (log_ != nullptr) {
    log_->println("{\"event\":\"wifi_manual_connect\"}");
  }
}

void WifiController::quiesceForProvisioning(Print &log) {
  // An asynchronous STA scan and an AP mode transition share the same radio.
  // Stop the scan at the ESP-IDF layer before changing Arduino WiFi mode so a
  // touch event can never race an in-flight scan/connect task.
  const esp_err_t scanStop = esp_wifi_scan_stop();
  WiFi.scanDelete();
  const wifi_mode_t mode = WiFi.getMode();
  const bool stationEnabled = (mode & WIFI_MODE_STA) != 0;
  const esp_err_t disconnectResult = stationEnabled
      ? esp_wifi_disconnect() : ESP_ERR_WIFI_NOT_CONNECT;
  radioOn_ = mode != WIFI_OFF;
  connected_ = false;
  scanning_ = false;
  connectionStartedMs_ = 0;
  scanStartedMs_ = 0;
  successfulNetworkNoted_ = false;
  powerSaveConfigured_ = false;
  powerSaveEnabled_ = false;
  log.printf(
      "{\"event\":\"wifi_quiesce_for_provisioning\",\"scan_stop\":%d,\"disconnect\":%d,\"mode\":%u}\n",
      static_cast<int>(scanStop), static_cast<int>(disconnectResult),
      static_cast<unsigned>(mode));
}

void WifiController::prepareForSleep() {
  stopRadio();
}

void WifiController::startConnection(uint32_t nowMs) {
  if (config_ == nullptr || !config_->hasWifi()) return;
  if (candidateOrder_.empty()) {
    startScan(nowMs);
    return;
  }
  connectCandidate(nowMs);
}

void WifiController::startScan(uint32_t nowMs) {
  WiFi.mode(WIFI_STA);
  setPowerSave(false);
  radioOn_ = true;
  connectionStartedMs_ = 0;
  connectionFailed_ = false;
  const int16_t scan = WiFi.scanNetworks(true, false, false, 120);
  scanning_ = scan == WIFI_SCAN_RUNNING;
  scanStartedMs_ = nowMs == 0 ? 1 : nowMs;
  if (scan >= 0) {
    buildCandidateOrder(scan);
    WiFi.scanDelete();
    scanning_ = false;
    connectCandidate(nowMs);
  } else if (!scanning_) {
    buildCandidateOrder(0);
    connectCandidate(nowMs);
  }
}

void WifiController::pollScan(uint32_t nowMs) {
  const int16_t count = WiFi.scanComplete();
  if (count == WIFI_SCAN_RUNNING &&
      static_cast<uint32_t>(nowMs - scanStartedMs_) < kWifiScanTimeoutMs) {
    return;
  }
  scanning_ = false;
  buildCandidateOrder(count < 0 ? 0 : count);
  WiFi.scanDelete();
  connectCandidate(nowMs);
}

void WifiController::buildCandidateOrder(int16_t scanCount) {
  std::vector<WifiCandidateScore> ranked;
  const auto &remembered = config_->wifiNetworks();
  ranked.reserve(remembered.size());
  for (size_t index = 0; index < remembered.size(); ++index) {
    WifiCandidateScore candidate;
    candidate.index = static_cast<uint8_t>(index);
    for (int16_t scanIndex = 0; scanIndex < scanCount; ++scanIndex) {
      if (WiFi.SSID(scanIndex) == remembered[index].ssid) {
        candidate.visible = true;
        candidate.rssi = std::max(candidate.rssi, WiFi.RSSI(scanIndex));
      }
    }
    ranked.push_back(candidate);
  }
  rankWifiCandidates(ranked);
  candidateOrder_.clear();
  for (const WifiCandidateScore &candidate : ranked) {
    candidateOrder_.push_back(candidate.index);
  }
  candidatePosition_ = 0;
}

void WifiController::connectCandidate(uint32_t nowMs) {
  if (config_ == nullptr || candidatePosition_ >= candidateOrder_.size()) return;
  const auto &remembered = config_->wifiNetworks();
  const size_t index = candidateOrder_[candidatePosition_];
  if (index >= remembered.size()) return;
  WiFi.mode(WIFI_STA);
  setPowerSave(false);
  clearWifiDisconnectReason();
  WiFi.begin(remembered[index].ssid.c_str(), remembered[index].password.c_str());
  radioOn_ = true;
  connectionStartedMs_ = nowMs == 0 ? 1 : nowMs;
  connectionFailed_ = false;
  successfulNetworkNoted_ = false;
  lastMruPersistAttemptMs_ = 0;
  log_->printf(
      "{\"event\":\"wifi_connecting\",\"round\":%u,\"candidate\":%u,\"remembered\":%u}\n",
      static_cast<unsigned>(failedAttempts_ + 1),
      static_cast<unsigned>(candidatePosition_ + 1),
      static_cast<unsigned>(candidateOrder_.size()));
}

void WifiController::stopRadio() {
  if (!radioOn_ && WiFi.getMode() == WIFI_OFF) return;
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);
  radioOn_ = false;
  powerSaveConfigured_ = false;
  powerSaveEnabled_ = false;
  connected_ = false;
  connectionStartedMs_ = 0;
  ntpStarted_ = false;
  scanning_ = false;
  WiFi.scanDelete();
}

void WifiController::setPowerSave(bool enabled) {
  if (powerSaveConfigured_ && powerSaveEnabled_ == enabled) return;
  const esp_err_t error = esp_wifi_set_ps(
      enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
  powerSaveError_ = error;
  if (error == ESP_OK) {
    powerSaveConfigured_ = true;
    powerSaveEnabled_ = enabled;
    if (log_ != nullptr) {
      log_->printf("{\"event\":\"wifi_power_save\",\"enabled\":%s}\n",
                   enabled ? "true" : "false");
    }
  }
}

void WifiController::noteFailure(uint32_t nowMs) {
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);
  radioOn_ = false;
  powerSaveConfigured_ = false;
  powerSaveEnabled_ = false;
  connected_ = false;
  connectionStartedMs_ = 0;
  successfulNetworkNoted_ = false;
  if (candidatePosition_ + 1 < candidateOrder_.size()) {
    ++candidatePosition_;
    connectionFailed_ = false;
    retryAtMs_ = nowMs + kWifiCandidateDelayMs;
    log_->printf("{\"event\":\"wifi_candidate_retry\",\"candidate\":%u}\n",
                 static_cast<unsigned>(candidatePosition_ + 1));
    return;
  }
  candidateOrder_.clear();
  candidatePosition_ = 0;
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

void WifiController::handleTimeSyncNotification() {
  timeSyncState_.noteSynchronized();
}

uint16_t WifiController::lastDisconnectReason() const {
  return lastWifiDisconnectReason();
}

}  // namespace pokepod
