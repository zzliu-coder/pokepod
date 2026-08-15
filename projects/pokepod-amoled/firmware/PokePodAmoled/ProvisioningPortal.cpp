#include "ProvisioningPortal.h"

#include <algorithm>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_wifi.h>

#include "ProvisioningPolicy.h"
#include "DeviceSecretWipe.h"
#include "MonotonicTime.h"
#include "WifiDisconnectDiagnostics.h"
#include "WifiFailurePolicy.h"

namespace pokepod {

void BoundedProvisioningWebServer::handleClient() {
  if (_currentStatus == HC_NONE) {
    _currentClient = _server.accept();
    if (!_currentClient) return;
    _currentClient.setTimeout(kIoSliceMs);
    _currentStatus = HC_WAIT_READ;
    _statusChange = millis();
  }

  bool keepCurrentClient = false;
  if (_currentClient.connected()) {
    if (_currentStatus == HC_WAIT_READ) {
      if (_currentClient.available()) {
        _currentClient.setTimeout(kIoSliceMs);
        const bool probeRequest = diagnostics_ != nullptr && log_ != nullptr &&
            requestProbeCount_ < 8;
        if (probeRequest) {
          diagnostics_->recordProbe(
              ProvisioningProbeStage::beforeRequestParse, *log_);
        }
        const bool parsed = _parseRequest(_currentClient);
        if (probeRequest) {
          diagnostics_->recordProbe(
              ProvisioningProbeStage::afterRequestParse, *log_);
          ++requestProbeCount_;
        }
        if (parsed) {
          _contentLength = CONTENT_LENGTH_NOT_SET;
          _responseCode = 0;
          _clearResponseHeaders();
          if (_chain != nullptr) {
            _chain->runChain(*this, [this]() { return _handleRequest(); });
          } else {
            _handleRequest();
          }
          if (_currentClient.isSSE()) {
            _currentStatus = HC_WAIT_CLOSE;
            _statusChange = millis();
            keepCurrentClient = true;
          }
        }
      } else if (millis() - _statusChange <= kIdleClientLifetimeMs) {
        keepCurrentClient = true;
      }
    } else if (_currentStatus == HC_WAIT_CLOSE &&
               _currentClient.isSSE() &&
               millis() - _statusChange <= kIdleClientLifetimeMs) {
      keepCurrentClient = true;
    }
  }

  if (!keepCurrentClient) {
    _currentClient = NetworkClient();
    _currentStatus = HC_NONE;
    _currentUpload.reset();
    _currentRaw.reset();
  } else {
    yield();
  }
}
namespace {

constexpr uint32_t kPortalLifetimeMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kValidationTimeoutMs = 15000;
constexpr uint32_t kScanTimeoutMs = 8000;
constexpr size_t kMaximumNetworks = 20;
constexpr size_t kProvisioningCsrfBytes = 32;
constexpr char kProvisioningCsrfHeader[] = "X-PokePod-CSRF";

bool fillProvisioningRandom(void *, uint8_t *destination, size_t length) {
  if (destination == nullptr || length == 0U) return false;
  esp_fill_random(destination, length);
  return true;
}

std::string newProvisioningCsrfToken() {
  static constexpr char hex[] = "0123456789abcdef";
  uint8_t bytes[kProvisioningCsrfBytes];
  esp_fill_random(bytes, sizeof(bytes));
  std::string token(sizeof(bytes) * 2, '0');
  for (size_t index = 0; index < sizeof(bytes); ++index) {
    token[index * 2] = hex[bytes[index] >> 4];
    token[index * 2 + 1] = hex[bytes[index] & 0x0f];
  }
  secureWipeBytes(bytes, sizeof(bytes));
  return token;
}

void logProvisioningMemory(Print &log, const char *phase) {
  log.printf(
      "{\"event\":\"provisioning_memory\",\"phase\":\"%s\",\"internal_free\":%u,\"internal_largest\":%u,\"psram_free\":%u}\n",
      phase,
      static_cast<unsigned>(heap_caps_get_free_size(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(ESP.getFreePsram()));
}

String validationFailureMessage(uint16_t reason) {
  switch (wifiFailureKind(reason)) {
    case WifiFailureKind::noAccessPoint:
      return "找不到该热点；请确认 2.4 GHz 热点仍开启";
    case WifiFailureKind::authentication:
      return "热点认证失败；请检查密码和 WPA2 兼容性";
    case WifiFailureKind::capacity:
      return "热点连接设备数已满";
    case WifiFailureKind::timeout:
      return "连接热点超时；请保持热点设置页开启后重试";
    case WifiFailureKind::association:
      return "热点拒绝连接；请检查热点兼容设置";
    case WifiFailureKind::other:
      return "无法连接该 Wi-Fi（原因 " + String(reason) + "）";
    case WifiFailureKind::none:
      return "无法连接该 Wi-Fi；没有收到热点响应";
  }
  return "无法连接该 Wi-Fi";
}

}  // namespace

ProvisioningPortal::ProvisioningPortal() : server_(80) {}

ProvisioningPortal::~ProvisioningPortal() {
  clearProvisioningCredential();
}

bool ProvisioningPortal::prepare(DeviceConfig &config,
                                 ProvisioningDiagnostics &diagnostics,
                                 Print &log) {
  if (active_ || prepared_) return true;
  // A fresh session must never inherit bytes from a previous terminal path.
  clearProvisioningCredential();
  config_ = &config;
  diagnostics_ = &diagnostics;
  log_ = &log;
  server_.attachProvisioningProbe(diagnostics, log);
  diagnostics_->recordProbe(ProvisioningProbeStage::prepareEntered, log);
  candidate_ = config.settings();
  clearCandidateSecrets();
  const uint32_t suffix = static_cast<uint32_t>(ESP.getEfuseMac() & 0xffff);
  char name[24];
  snprintf(name, sizeof(name), "PokePod-%04lX", static_cast<unsigned long>(suffix));
  ssid_ = name;
  if (!credential_.begin(fillProvisioningRandom, nullptr)) {
    clearProvisioningCredential();
    statusMessage_ = "配网密码失败，请退出后重试";
    return false;
  }
  password_ = credential_.password();
  // Keep the captive AP stable while the phone loads the page. Scanning on
  // the single ESP32 radio is explicit (the user can tap 重新扫描) so the
  // first HTTP request never competes with an STA scan.
  statusMessage_ = "正在准备配网热点";
  validating_ = false;
  scanning_ = false;
  transitionPending_ = false;
  changed_ = false;
  saved_ = false;
  closeAtMs_ = 0;
  candidateRssi_ = -127;
  validationAttempt_ = 0;
  sensitiveConfirmation_.reset();
  csrf_.begin(newProvisioningCsrfToken(), millis(), kPortalLifetimeMs);
  prepared_ = true;
  starting_ = true;
  diagnostics_->record(ProvisioningLogStage::portalRequested,
                       ProvisioningLogOutcome::info, ssid_, 0, 0, 0, 0,
                       log);
  diagnostics_->recordProbe(ProvisioningProbeStage::portalRequested, log);
  log.printf("{\"event\":\"provisioning\",\"phase\":\"requested\",\"ssid\":\"%s\"}\n",
             ssid_.c_str());
  return true;
}

bool ProvisioningPortal::switchToAccessPointMode() {
  if (active_) return true;
  if (!prepared_ || diagnostics_ == nullptr || log_ == nullptr) {
    clearProvisioningCredential();
    return false;
  }
  const wifi_mode_t mode = WiFi.getMode();
  if ((mode != WIFI_OFF && mode != WIFI_STA) ||
      WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
    starting_ = false;
    statusMessage_ = "无线网络仍在关闭，请退出后重试";
    diagnostics_->record(ProvisioningLogStage::failed,
                         ProvisioningLogOutcome::failure, ssid_, 0,
                         kProvisioningReasonRadioBusy, 0, 0, *log_);
    clearProvisioningCredential();
    return false;
  }
  logProvisioningMemory(*log_, "before-mode-ap");
  diagnostics_->recordProbe(ProvisioningProbeStage::beforeModeAp, *log_);
  if (!WiFi.mode(WIFI_AP)) {
    starting_ = false;
    statusMessage_ = "无线模式切换失败，请退出后重试";
    diagnostics_->record(ProvisioningLogStage::failed,
                         ProvisioningLogOutcome::failure, ssid_, 0,
                         kProvisioningReasonPortalFailed, 0, 0, *log_);
    log_->println(
        "{\"event\":\"provisioning\",\"ok\":false,\"stage\":\"mode-ap\"}");
    clearProvisioningCredential();
    return false;
  }
  diagnostics_->recordProbe(ProvisioningProbeStage::afterModeAp, *log_);
  diagnostics_->record(ProvisioningLogStage::radioModeStarted,
                       ProvisioningLogOutcome::success, ssid_, 0, 0, 0, 0,
                       *log_);
  logProvisioningMemory(*log_, "after-mode-ap");
  return true;
}

bool ProvisioningPortal::startAccessPoint() {
  if (active_) return true;
  if (!prepared_ || diagnostics_ == nullptr || log_ == nullptr ||
      (WiFi.getMode() & WIFI_MODE_AP) == 0) {
    clearProvisioningCredential();
    return false;
  }
  logProvisioningMemory(*log_, "before-softap");
  diagnostics_->recordProbe(ProvisioningProbeStage::beforeSoftAp, *log_);
  if (!WiFi.softAP(ssid_.c_str(), password_.c_str())) {
    starting_ = false;
    statusMessage_ = "配网热点启动失败，请退出后重试";
    diagnostics_->record(ProvisioningLogStage::failed,
                         ProvisioningLogOutcome::failure, ssid_, 0,
                         kProvisioningReasonPortalFailed, 0, 0, *log_);
    log_->println(
        "{\"event\":\"provisioning\",\"ok\":false,\"stage\":\"softap\"}");
    clearProvisioningCredential();
    return false;
  }
  diagnostics_->recordProbe(ProvisioningProbeStage::afterSoftAp, *log_);
  diagnostics_->recordProbe(
      ProvisioningProbeStage::beforePowerSaveOff, *log_);
  esp_wifi_set_ps(WIFI_PS_NONE);
  diagnostics_->recordProbe(
      ProvisioningProbeStage::afterPowerSaveOff, *log_);
  diagnostics_->record(ProvisioningLogStage::accessPointStarted,
                       ProvisioningLogOutcome::success, ssid_, 0, 0, 0, 0,
                       *log_);
  logProvisioningMemory(*log_, "after-softap");
  return true;
}

bool ProvisioningPortal::startServices() {
  if (active_) return true;
  if (!prepared_ || diagnostics_ == nullptr || log_ == nullptr ||
      (WiFi.getMode() & WIFI_MODE_AP) == 0) {
    clearProvisioningCredential();
    return false;
  }
  logProvisioningMemory(*log_, "before-services");
  diagnostics_->recordProbe(
      ProvisioningProbeStage::beforeRouteInstall, *log_);
  installRoutes();
  diagnostics_->recordProbe(
      ProvisioningProbeStage::afterRouteInstall, *log_);
  diagnostics_->recordProbe(ProvisioningProbeStage::beforeDnsStart, *log_);
  dns_.start(53, "*", WiFi.softAPIP());
  diagnostics_->recordProbe(ProvisioningProbeStage::afterDnsStart, *log_);
  diagnostics_->recordProbe(
      ProvisioningProbeStage::beforeServerBegin, *log_);
  server_.begin();
  diagnostics_->recordProbe(
      ProvisioningProbeStage::afterServerBegin, *log_);
  startedMs_ = millis();
  csrf_.begin(csrf_.token(), startedMs_, kPortalLifetimeMs);
  active_ = true;
  starting_ = false;
  statusMessage_ = "请选择附近的 2.4 GHz 网络或手工输入";
  diagnostics_->record(ProvisioningLogStage::portalStarted,
                       ProvisioningLogOutcome::success, ssid_, 0, 0, 0, 0,
                       *log_);
  log_->printf("{\"event\":\"provisioning\",\"ok\":true,\"ssid\":\"%s\",\"expires_ms\":%lu}\n",
               ssid_.c_str(), static_cast<unsigned long>(kPortalLifetimeMs));
  logProvisioningMemory(*log_, "after-services");
  return true;
}

void ProvisioningPortal::failStartupTimeout() {
  if (!prepared_ || diagnostics_ == nullptr || log_ == nullptr) {
    clearProvisioningCredential();
    return;
  }
  starting_ = false;
  statusMessage_ = "无线网络关闭超时，请退出后重试";
  diagnostics_->record(ProvisioningLogStage::failed,
                       ProvisioningLogOutcome::failure, ssid_, 0,
                       kProvisioningReasonStartupTimeout, 0, 0, *log_);
  clearProvisioningCredential();
}

void ProvisioningPortal::loop(uint32_t nowMs) {
  if (!active_) return;
  ProvisioningPollScope pollScope(diagnostics_, log_);
  dns_.processNextRequest();
  server_.handleClient();
  // HTTP handlers can create a new validation timestamp. Refresh the clock
  // after handling the request so this poll can never appear to predate it.
  nowMs = millis();
  if (sensitiveConfirmation_.expire(nowMs)) {
    discardSensitiveCandidate();
    statusMessage_ = "实体确认已超时，请重新保存";
  }
  if (transitionPending_ &&
      static_cast<int32_t>(nowMs - transitionAtMs_) >= 0) {
    transitionPending_ = false;
    beginStationValidation();
  }
  pollScan();
  // A validation is visible to the web UI while its 202 response drains, but
  // Wi-Fi outcome/timeout handling starts only after beginStationValidation.
  if (validating_ && !transitionPending_) {
    if (WiFi.status() == WL_CONNECTED) {
      validating_ = false;
      const uint32_t elapsed =
          monotonicElapsedOrZero(nowMs, validatingSinceMs_);
      diagnostics_->record(ProvisioningLogStage::connected,
                           ProvisioningLogOutcome::success,
                           candidate_.wifiSsid, WiFi.RSSI(), 0, elapsed,
                           validationAttempt_, *log_);
      if (config_->save(candidate_, *log_)) {
        changed_ = true;
        saved_ = true;
        statusMessage_ = "Wi-Fi 已连接";
        closeAtMs_ = nowMs + 1800;
        diagnostics_->record(ProvisioningLogStage::configSaved,
                             ProvisioningLogOutcome::success,
                             candidate_.wifiSsid, WiFi.RSSI(), 0, elapsed,
                             validationAttempt_, *log_);
        clearCandidateSecrets();
      } else {
        saved_ = false;
        statusMessage_ = "写入配置失败，请重试";
        diagnostics_->record(ProvisioningLogStage::failed,
                             ProvisioningLogOutcome::failure,
                             candidate_.wifiSsid, WiFi.RSSI(),
                             kProvisioningReasonStorage, elapsed,
                             validationAttempt_, *log_);
        restorePortalForRetry();
      }
    } else if (monotonicElapsedAtLeast(nowMs, validatingSinceMs_,
                                       kValidationTimeoutMs)) {
      validating_ = false;
      saved_ = false;
      const uint16_t reason = lastWifiDisconnectReason();
      statusMessage_ = validationFailureMessage(reason);
      diagnostics_->record(ProvisioningLogStage::failed,
                           ProvisioningLogOutcome::failure,
                           candidate_.wifiSsid, candidateRssi_, reason,
                           monotonicElapsedOrZero(nowMs, validatingSinceMs_),
                           validationAttempt_,
                           *log_);
      restorePortalForRetry();
    }
  }
  if ((closeAtMs_ != 0 && static_cast<int32_t>(nowMs - closeAtMs_) >= 0) ||
      monotonicElapsedAtLeast(nowMs, startedMs_, kPortalLifetimeMs)) {
    stop();
  }
}

void ProvisioningPortal::stop() {
  const bool wasActive = active_;
  const bool wasPrepared = prepared_;
  if (wasActive) {
    dns_.stop();
    server_.stop();
  }
  if (wasPrepared) {
    if ((WiFi.getMode() & WIFI_MODE_AP) != 0) {
      WiFi.softAPdisconnect(true);
    }
    WiFi.disconnect(false, false);
    WiFi.scanDelete();
    // Leave the driver initialized until WifiController resumes on the next
    // loop. This avoids an immediate deinit/reinit cycle when the user closes
    // and reopens provisioning.
    WiFi.mode(WIFI_STA);
  }
  active_ = false;
  prepared_ = false;
  starting_ = false;
  validating_ = false;
  scanning_ = false;
  transitionPending_ = false;
  networks_.clear();
  closeAtMs_ = 0;
  csrf_.close();
  sensitiveConfirmation_.reset();
  clearCandidateSecrets();
  candidate_ = DeviceSettings{};
  clearProvisioningCredential();
  if ((wasActive || wasPrepared) && diagnostics_ != nullptr && log_ != nullptr) {
    diagnostics_->recordProbe(ProvisioningProbeStage::portalStopped, *log_);
    diagnostics_->record(ProvisioningLogStage::portalStopped,
                         ProvisioningLogOutcome::info, ssid_, 0, 0,
                         wasActive ? millis() - startedMs_ : 0,
                         validationAttempt_, *log_);
  }
  if ((wasActive || wasPrepared) && log_ != nullptr) {
    log_->println("{\"event\":\"provisioning_stopped\"}");
  }
}

void ProvisioningPortal::clearProvisioningCredential() {
  clearCandidateSecrets();
  csrf_.close();
  secureWipe(password_);
  credential_.close();
}

void ProvisioningPortal::clearCandidateSecrets() {
  secureWipeSecrets(candidate_);
}

bool ProvisioningPortal::takeConfigurationChanged() {
  if (active_) return false;
  const bool value = changed_;
  changed_ = false;
  return value;
}

void ProvisioningPortal::installRoutes() {
  if (routesInstalled_) return;
  const char *headers[] = {kProvisioningCsrfHeader};
  server_.collectHeaders(headers, 1);
  server_.on("/", HTTP_GET, [this]() { showPortal(); });
  server_.on("/networks", HTTP_GET, [this]() { showNetworks(); });
  server_.on("/scan", HTTP_POST, [this]() { scanRequest(); });
  server_.on("/save", HTTP_POST, [this]() { saveRequest(); });
  server_.on("/forget-network", HTTP_POST, [this]() { forgetRequest(); });
  server_.on("/status", HTTP_GET, [this]() { showPortal(); });
  server_.on("/generate_204", HTTP_ANY, [this]() { redirectPortal(); });
  server_.on("/hotspot-detect.html", HTTP_ANY, [this]() { redirectPortal(); });
  server_.on("/connecttest.txt", HTTP_ANY, [this]() { redirectPortal(); });
  server_.onNotFound([this]() { redirectPortal(); });
  routesInstalled_ = true;
}

void ProvisioningPortal::startScan() {
  if (!active_ || validating_ || scanning_) return;
  WiFi.mode(WIFI_AP_STA);
  const int16_t result = WiFi.scanNetworks(true, false, false, 120);
  scanning_ = result == WIFI_SCAN_RUNNING;
  scanStartedMs_ = millis();
  if (diagnostics_ != nullptr && log_ != nullptr) {
    diagnostics_->record(ProvisioningLogStage::scanStarted,
                         ProvisioningLogOutcome::info, "", 0, 0,
                         scanStartedMs_ - startedMs_, validationAttempt_,
                         *log_);
  }
  if (!scanning_) {
    WiFi.mode(WIFI_AP);
    statusMessage_ = "扫描启动失败；可手工输入网络名称";
    if (log_ != nullptr) {
      log_->println("{\"event\":\"wifi_scan\",\"ok\":false,\"stage\":\"start\"}");
    }
    if (diagnostics_ != nullptr && log_ != nullptr) {
      diagnostics_->record(ProvisioningLogStage::failed,
                           ProvisioningLogOutcome::failure, "", 0,
                           kProvisioningReasonScanStart,
                           millis() - scanStartedMs_, validationAttempt_,
                           *log_);
    }
  }
}

void ProvisioningPortal::pollScan() {
  if (!scanning_) return;
  const int16_t count = WiFi.scanComplete();
  if (count == WIFI_SCAN_RUNNING) {
    if (static_cast<uint32_t>(millis() - scanStartedMs_) < kScanTimeoutMs) return;
    scanning_ = false;
    WiFi.scanDelete();
    WiFi.mode(WIFI_AP);
    statusMessage_ = "扫描超时；可重新扫描或手工输入";
    if (log_ != nullptr) {
      log_->println("{\"event\":\"wifi_scan\",\"ok\":false,\"stage\":\"timeout\"}");
    }
    if (diagnostics_ != nullptr && log_ != nullptr) {
      diagnostics_->record(ProvisioningLogStage::failed,
                           ProvisioningLogOutcome::failure, "", 0,
                           kProvisioningReasonScanTimeout,
                           millis() - scanStartedMs_, validationAttempt_,
                           *log_);
    }
    return;
  }
  if (count < 0) {
    scanning_ = false;
    WiFi.scanDelete();
    WiFi.mode(WIFI_AP);
    statusMessage_ = "扫描失败；可重新扫描或手工输入";
    if (log_ != nullptr) {
      log_->println("{\"event\":\"wifi_scan\",\"ok\":false,\"stage\":\"complete\"}");
    }
    if (diagnostics_ != nullptr && log_ != nullptr) {
      diagnostics_->record(ProvisioningLogStage::failed,
                           ProvisioningLogOutcome::failure, "", 0,
                           kProvisioningReasonScanFailed,
                           millis() - scanStartedMs_, validationAttempt_,
                           *log_);
    }
    return;
  }

  networks_.clear();
  for (int16_t index = 0; index < count; ++index) {
    const String ssid = WiFi.SSID(index);
    if (ssid.isEmpty()) continue;
    const int32_t rssi = WiFi.RSSI(index);
    const bool secured = WiFi.encryptionType(index) != WIFI_AUTH_OPEN;
    auto existing = std::find_if(networks_.begin(), networks_.end(),
        [&ssid](const ScannedNetwork &network) { return network.ssid == ssid; });
    if (existing == networks_.end()) {
      networks_.push_back({ssid, rssi, secured});
    } else if (rssi > existing->rssi) {
      existing->rssi = rssi;
      existing->secured = secured;
    }
  }
  std::sort(networks_.begin(), networks_.end(),
      [](const ScannedNetwork &left, const ScannedNetwork &right) {
        return left.rssi > right.rssi;
      });
  if (networks_.size() > kMaximumNetworks) networks_.resize(kMaximumNetworks);
  WiFi.scanDelete();
  scanning_ = false;
  // Return to AP-only mode after the scan. This prevents the STA side from
  // stealing the AP channel while the phone is using the portal.
  WiFi.mode(WIFI_AP);
  statusMessage_ = networks_.empty()
      ? "没有发现网络；可重新扫描或手工输入"
      : "请选择附近的 2.4 GHz 网络";
  if (log_ != nullptr) {
    log_->printf("{\"event\":\"wifi_scan\",\"ok\":true,\"networks\":%u}\n",
                 static_cast<unsigned>(networks_.size()));
  }
  if (diagnostics_ != nullptr && log_ != nullptr) {
    diagnostics_->record(ProvisioningLogStage::scanFinished,
                         ProvisioningLogOutcome::success, "", 0, 0,
                         millis() - scanStartedMs_, validationAttempt_,
                         *log_);
  }
}

void ProvisioningPortal::showNetworks() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json; charset=utf-8", networksJson());
}

void ProvisioningPortal::scanRequest() {
  if (!authorizeMutation()) return;
  if (validating_ || sensitiveConfirmation_.pending()) {
    server_.send(409, "application/json; charset=utf-8",
                 sensitiveConfirmation_.pending()
                     ? "{\"error\":\"请先在设备上确认腾讯密钥操作\"}"
                     : "{\"error\":\"正在验证 Wi-Fi\"}");
    return;
  }
  startScan();
  showNetworks();
}

void ProvisioningPortal::showPortal() {
  server_.sendHeader("Cache-Control", "no-store");
  if (diagnostics_ != nullptr && log_ != nullptr) {
    diagnostics_->recordProbe(ProvisioningProbeStage::beforePageBuild, *log_);
  }
  const String html = pageHtml();
  if (diagnostics_ != nullptr && log_ != nullptr) {
    diagnostics_->recordProbe(ProvisioningProbeStage::afterPageBuild, *log_);
    diagnostics_->recordProbe(ProvisioningProbeStage::beforePageSend, *log_);
  }
  server_.send(200, "text/html; charset=utf-8", html);
  if (diagnostics_ != nullptr && log_ != nullptr) {
    diagnostics_->recordProbe(ProvisioningProbeStage::afterPageSend, *log_);
  }
}

void ProvisioningPortal::saveRequest() {
  if (!authorizeMutation()) return;
  if (sensitiveConfirmation_.pending()) {
    statusMessage_ = "请先按下设备实体键确认腾讯密钥操作";
    sendSaveJson(409, false);
    return;
  }
  if (validating_) {
    statusMessage_ = "正在验证 Wi-Fi，请稍候";
    sendSaveJson(409, false);
    return;
  }
  if (scanning_) {
    statusMessage_ = "正在扫描附近网络，请稍候";
    sendSaveJson(409, false);
    return;
  }
  DeviceSettings next = config_->settings();
  const String manualSsid = server_.arg("ssidManual");
  next.wifiSsid = manualSsid.isEmpty() ? server_.arg("ssid") : manualSsid;
  next.wifiPassword = server_.arg("wifiPassword");
  if (next.wifiPassword.isEmpty()) {
    const WifiCredential *remembered = config_->wifiNetwork(next.wifiSsid);
    if (remembered != nullptr) next.wifiPassword = remembered->password;
  }
  next.hotwordId = server_.arg("hotwordId");
  next.wifiEnabled = true;
  String secretId = server_.arg("secretId");
  String secretKey = server_.arg("secretKey");
  const auto clearRequestSecrets = [&]() {
    secureWipe(secretId);
    secureWipe(secretKey);
    secureWipeSecrets(next);
  };
  ProvisioningSensitiveAction sensitiveAction =
      ProvisioningSensitiveAction::none;
  if (server_.hasArg("clearTencent")) {
    if (config_->hasTencent()) {
      sensitiveAction = ProvisioningSensitiveAction::clearTencent;
    }
    next.secretId = "";
    next.secretKey = "";
    next.hotwordId = "";
  } else if (!secretId.isEmpty() || !secretKey.isEmpty()) {
    if (secretId.isEmpty() || secretKey.isEmpty()) {
      statusMessage_ = "SecretId 和 SecretKey 必须同时填写";
      diagnostics_->record(ProvisioningLogStage::failed,
                           ProvisioningLogOutcome::failure, next.wifiSsid,
                           -127, kProvisioningReasonInvalidInput, 0,
                           validationAttempt_, *log_);
      sendSaveJson(400, false);
      clearRequestSecrets();
      return;
    }
    const DeviceSettings &current = config_->settings();
    if (secretId != current.secretId || secretKey != current.secretKey) {
      sensitiveAction = ProvisioningSensitiveAction::replaceTencent;
    }
    next.secretId = secretId;
    next.secretKey = secretKey;
  }
  if (next.wifiSsid.isEmpty() || next.wifiSsid.length() > 32 ||
      (!next.wifiPassword.isEmpty() &&
       (next.wifiPassword.length() < 8 || next.wifiPassword.length() > 63))) {
    statusMessage_ = "Wi-Fi 名称或密码长度不正确";
    diagnostics_->record(ProvisioningLogStage::failed,
                         ProvisioningLogOutcome::failure, next.wifiSsid,
                         -127, kProvisioningReasonInvalidInput, 0,
                         validationAttempt_, *log_);
    sendSaveJson(400, false);
    clearRequestSecrets();
    return;
  }
  clearCandidateSecrets();
  candidate_ = next;
  clearRequestSecrets();
  candidateRssi_ = -127;
  const auto scanned = std::find_if(
      networks_.begin(), networks_.end(),
      [&next](const ScannedNetwork &network) {
        return network.ssid == next.wifiSsid;
      });
  if (scanned != networks_.end()) candidateRssi_ = scanned->rssi;
  ++validationAttempt_;
  saved_ = false;
  if (sensitiveAction != ProvisioningSensitiveAction::none) {
    if (!sensitiveConfirmation_.begin(sensitiveAction, millis())) {
      discardSensitiveCandidate();
      statusMessage_ = "实体确认无法启动，请重新保存";
      sendSaveJson(500, false);
      return;
    }
    statusMessage_ = sensitiveAction == ProvisioningSensitiveAction::clearTencent
        ? "请按下设备实体键确认清除腾讯密钥"
        : "请按下设备实体键确认更换腾讯密钥";
    sendSaveJson(202, true);
    return;
  }
  armStationValidation(millis());
  sendSaveJson(202, true);
}

void ProvisioningPortal::armStationValidation(uint32_t nowMs) {
  // This compact device status uses the fixed 16 px font. Keep the runtime
  // SSID in the full-font diagnostics and phone portal so arbitrary network
  // names can never render as missing glyph boxes here.
  statusMessage_ = "正在连接网络";
  validating_ = true;
  validatingSinceMs_ = nowMs;
  transitionPending_ = true;
  transitionAtMs_ = validatingSinceMs_ + 200;
}

bool ProvisioningPortal::confirmSensitiveChange(uint32_t nowMs) {
  if (!active_) return false;
  if (monotonicElapsedAtLeast(nowMs, startedMs_, kPortalLifetimeMs)) {
    discardSensitiveCandidate();
    stop();
    return false;
  }
  if (!sensitiveConfirmation_.acceptPhysicalPress(nowMs)) {
    discardSensitiveCandidate();
    return false;
  }
  armStationValidation(nowMs);
  statusMessage_ = "实体确认完成，正在连接网络";
  return true;
}

void ProvisioningPortal::discardSensitiveCandidate() {
  sensitiveConfirmation_.reset();
  clearCandidateSecrets();
  if (config_ != nullptr) {
    candidate_ = config_->settings();
  } else {
    candidate_ = DeviceSettings{};
  }
  clearCandidateSecrets();
}

void ProvisioningPortal::beginStationValidation() {
  dns_.stop();
  WiFi.softAPdisconnect(false);
  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
  validatingSinceMs_ = millis();
  clearWifiDisconnectReason();
  if (diagnostics_ != nullptr && log_ != nullptr) {
    diagnostics_->record(ProvisioningLogStage::connectStarted,
                         ProvisioningLogOutcome::info,
                         candidate_.wifiSsid, candidateRssi_, 0, 0,
                         validationAttempt_, *log_);
  }
  WiFi.begin(candidate_.wifiSsid.c_str(), candidate_.wifiPassword.c_str());
}

void ProvisioningPortal::restorePortalForRetry() {
  transitionPending_ = false;
  dns_.stop();
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP);
  esp_wifi_set_ps(WIFI_PS_NONE);
  if (WiFi.softAP(ssid_.c_str(), password_.c_str())) {
    dns_.start(53, "*", WiFi.softAPIP());
  }
  discardSensitiveCandidate();
}

void ProvisioningPortal::forgetRequest() {
  if (!authorizeMutation()) return;
  if (validating_ || sensitiveConfirmation_.pending()) {
    server_.send(409, "application/json; charset=utf-8",
                 sensitiveConfirmation_.pending()
                     ? "{\"error\":\"请先在设备上确认腾讯密钥操作\"}"
                     : "{\"error\":\"正在验证 Wi-Fi\"}");
    return;
  }
  const String ssid = server_.arg("ssid");
  if (ssid.isEmpty() || config_->wifiNetwork(ssid) == nullptr) {
    server_.send(404, "application/json; charset=utf-8",
                 "{\"error\":\"没有找到该网络\"}");
    return;
  }
  if (!config_->forgetWifi(ssid, *log_)) {
    server_.send(500, "application/json; charset=utf-8",
                 "{\"error\":\"保存失败，请重试\"}");
    return;
  }
  candidate_ = config_->settings();
  clearCandidateSecrets();
  changed_ = true;
  statusMessage_ = "已忘记所选网络";
  showNetworks();
}

bool ProvisioningPortal::authorizeMutation() {
  String candidate = server_.header(kProvisioningCsrfHeader);
  std::string token(candidate.c_str());
  const bool accepted = csrf_.accepts(token, millis());
  secureWipe(token);
  secureWipe(candidate);
  if (accepted) return true;
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(403, "application/json; charset=utf-8",
               "{\"error\":\"配网页无法使用，请重新进入手机配网\"}");
  return false;
}

void ProvisioningPortal::sendSaveJson(int statusCode, bool accepted) {
  String json;
  json.reserve(statusMessage_.length() + 180);
  json += "{\"accepted\":";
  json += accepted ? "true" : "false";
  json += ",\"validating\":";
  json += validating_ ? "true" : "false";
  json += ",\"saved\":";
  json += saved_ ? "true" : "false";
  json += ",\"confirmationRequired\":";
  json += sensitiveConfirmation_.pending() ? "true" : "false";
  json += ",\"confirmationAction\":\"";
  json += provisioningSensitiveActionName(sensitiveConfirmation_.action());
  json += "\",\"confirmationRemainingSeconds\":";
  json += String((sensitiveConfirmation_.remainingMs(millis()) + 999U) / 1000U);
  json += ",\"wifiState\":\"";
  json += portalState();
  json += "\"";
  json += ",\"message\":\"" + jsonEscape(statusMessage_) + "\"}";
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(statusCode, "application/json; charset=utf-8", json);
}

ProvisioningState ProvisioningPortal::state() const {
  if (saved_) return ProvisioningState::connected;
  if (starting_ || validating_ || transitionPending_ ||
      sensitiveConfirmation_.pending()) {
    return ProvisioningState::connecting;
  }
  if (scanning_) return ProvisioningState::scanning;
  if (statusMessage_.indexOf("失败") >= 0 ||
      statusMessage_.indexOf("无法") >= 0 ||
      statusMessage_.indexOf("超时") >= 0 ||
      statusMessage_.indexOf("仍在") >= 0 ||
      statusMessage_.indexOf("拒绝") >= 0 ||
      statusMessage_.indexOf("找不到") >= 0) {
    return ProvisioningState::error;
  }
  return ProvisioningState::ready;
}

const char *ProvisioningPortal::portalState() const {
  return provisioningStateName(state());
}

void ProvisioningPortal::redirectPortal() {
  server_.sendHeader("Location", "http://192.168.4.1/", true);
  server_.send(302, "text/plain", "");
}

String ProvisioningPortal::pageHtml() const {
  String html;
  html.reserve(16000);
  html += F(R"HTML(<!doctype html>
<html lang='zh-CN'>
<head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>
<meta name='theme-color' content='#000000'>
<title>PokePod 设置</title>
<style>
:root{color-scheme:dark;--viewport-height:100dvh}
*{box-sizing:border-box}
html{width:100%;min-height:100%;background:#000;-webkit-text-size-adjust:100%;scroll-behavior:auto}
body{width:100%;min-height:100%;margin:0;background:#000;color:#f4faf7;font-family:-apple-system,BlinkMacSystemFont,"SF Pro Text","PingFang SC",sans-serif;overflow-wrap:anywhere}
button,input,select{font:inherit}
button{touch-action:manipulation}
.shell{width:100%;max-width:520px;min-height:100vh;min-height:var(--viewport-height);margin:0 auto;padding:calc(20px + env(safe-area-inset-top)) 18px calc(34px + env(safe-area-inset-bottom))}
.top{display:flex;align-items:center;justify-content:space-between}
.brand{display:flex;align-items:center;gap:10px;color:#91a69f;font-size:13px;font-weight:750;letter-spacing:.08em}
.mark{position:relative;width:32px;height:20px;border:2px solid #69e0b6;border-radius:999px}
.mark:after{content:"";position:absolute;left:15px;top:2px;width:1px;height:12px;background:#69e0b6}
.progress{display:flex;gap:7px}
.progress i{display:block;width:18px;height:3px;border-radius:9px;background:#34413c}
.progress i.on{background:#69e0b6}
.hero{padding:22px 2px 16px}
.hero h1{margin:0;font-size:30px;line-height:1.12;letter-spacing:-.03em}
.hero p{margin:8px 0 0;color:#91a69f;font-size:15px;line-height:1.45}
.status{display:flex;gap:10px;margin:0 0 14px;padding:11px 13px;border:1px solid #1a2a25;border-radius:14px;background:#0b1311;color:#cbd9d4;font-size:14px;line-height:1.45}
.status:before{content:"";flex:0 0 auto;width:8px;height:8px;margin-top:6px;border-radius:50%;background:#f0c45b;box-shadow:0 4px 14px rgba(240,196,91,.28)}
.status[data-state='connecting']:before{background:#f0c45b;box-shadow:0 0 14px rgba(240,196,91,.45);animation:pulse 1.2s ease-in-out infinite}
.status[data-state='connected']:before{background:#69e0b6;box-shadow:0 0 14px rgba(105,224,182,.45)}
.status[data-state='error']:before{background:#ff786d;box-shadow:0 0 14px rgba(255,120,109,.35)}
@keyframes pulse{50%{opacity:.35;transform:scale(.72)}}
.connection{display:flex;align-items:center;justify-content:space-between;gap:14px;margin:0 0 14px;padding:13px 14px;border:1px solid #1a2a25;border-radius:14px;background:#07100d}
.connection-copy{min-width:0}.connection-title{color:#91a69f;font-size:12px;font-weight:700}.connection-detail{margin-top:4px;color:#f4faf7;font-size:15px;font-weight:750;white-space:nowrap;text-overflow:ellipsis}
.connection-state{flex:0 0 auto;color:#f0c45b;font-size:14px;font-weight:800}.connection[data-state='connected'] .connection-state{color:#69e0b6}.connection[data-state='error'] .connection-state{color:#ff786d}
.screen[hidden]{display:none}
.card{padding:18px 16px;border:1px solid #1a2a25;border-radius:18px;background:#0b1311}
.card-head{display:flex;align-items:center;gap:12px;margin-bottom:17px}
.number{display:grid;place-items:center;width:38px;height:38px;border-radius:13px;background:#12372c;color:#69e0b6;font-size:14px;font-weight:850}
.card h2{margin:0;font-size:22px;line-height:1.2}
.saved{display:inline-flex;margin:0;padding:6px 10px;border-radius:999px;background:#12372c;color:#69e0b6;font-size:13px;font-weight:750}
.saved-row{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:4px}
.saved-copy{color:#91a69f;font-size:13px}
.field{display:block;margin-top:16px;scroll-margin:96px 0 120px}
.field:first-child{margin-top:0}
.field-name{display:block;margin-bottom:8px;color:#cbd9d4;font-size:14px;font-weight:700}
.optional{color:#91a69f;font-weight:500}
input,select{display:block;width:100%;min-height:52px;padding:13px 14px;border:1px solid #294038;border-radius:14px;outline:0;background:#030706;color:#f4faf7;font-size:16px;line-height:1.4}
input::placeholder{color:#60756d}
input:focus,select:focus{border-color:#69e0b6;box-shadow:0 0 0 3px rgba(105,224,182,.12)}
select{appearance:none;padding-right:40px;background-image:linear-gradient(45deg,transparent 50%,#91a69f 50%),linear-gradient(135deg,#91a69f 50%,transparent 50%);background-position:calc(100% - 19px) 23px,calc(100% - 14px) 23px;background-size:5px 5px;background-repeat:no-repeat}
.scan-row{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-top:11px}
.scan-help{color:#91a69f;font-size:12px}
.remembered{margin:0 0 16px;padding:12px;border:1px solid #1f342d;border-radius:14px;background:#07100d}
.remembered-title{margin:0 0 8px;color:#91a69f;font-size:12px;font-weight:700}
.remembered-item{display:flex;align-items:center;justify-content:space-between;gap:12px;min-height:38px;border-top:1px solid #14231e;color:#cbd9d4;font-size:14px}
.remembered-item:first-of-type{border-top:0}
.forget{border:0;background:transparent;color:#ff8d82;font-size:13px;font-weight:700}
.secondary{min-height:44px;padding:9px 14px;border:1px solid #2c4a40;border-radius:13px;background:#101a17;color:#69e0b6;font-size:14px;font-weight:750}
.secondary:disabled{opacity:.45}
.disclosure{margin-top:16px;border-top:1px solid #1a2a25;padding-top:14px}
.disclosure summary{min-height:44px;padding:11px 0;cursor:pointer;color:#cbd9d4;font-size:15px;font-weight:700;list-style:none}
.disclosure summary::-webkit-details-marker{display:none}
.disclosure summary:after{content:"＋";float:right;color:#69e0b6}
.disclosure[open] summary:after{content:"－"}
.disclosure-body{padding-top:4px}
.manual{margin-top:14px}
.credential-fields{padding-top:2px}
.actions{position:sticky;bottom:0;z-index:4;display:grid;grid-template-columns:1fr;gap:10px;margin:16px -4px 0;padding:12px 4px max(12px,env(safe-area-inset-bottom));background:#000;border-top:1px solid #101a17}
.actions.two{grid-template-columns:1fr 1.6fr}
.primary,.back{min-height:56px;border-radius:16px;font-size:17px;font-weight:820}
.primary{border:0;background:#69e0b6;color:#07110d}
.back{border:1px solid #294038;background:#101a17;color:#cbd9d4}
.primary:disabled,.back:disabled{opacity:.5}
.result{min-height:300px;display:grid;place-items:center;text-align:center;padding:32px 20px}
.result-mark{display:grid;place-items:center;width:72px;height:72px;margin:0 auto 20px;border-radius:50%;background:#12372c;color:#69e0b6;font-size:34px;font-weight:850}
.result h2{margin:0;font-size:24px}.result p{margin:10px 0 0;color:#91a69f;font-size:15px;line-height:1.55}
.danger{margin-top:16px;padding-top:14px;border-top:1px solid #1a2a25}
.check{display:flex;align-items:flex-start;gap:10px;color:#91a69f;font-size:13px;line-height:1.45}
.check input{flex:0 0 auto;width:20px;min-height:20px;height:20px;margin:0;accent-color:#ff786d}
.privacy{margin:16px 0 0;color:#91a69f;font-size:12px;line-height:1.55}
.footer{margin:18px 0 0;text-align:center;color:#52655e;font-size:12px}
@media(min-width:600px){.shell{padding-left:24px;padding-right:24px}.card{padding:22px}}
</style>
</head>
<body>
<main class='shell'>
<div class='top'><div class='brand'><span class='mark' aria-hidden='true'></span><span>POKEPOD</span></div><div class='progress' aria-hidden='true'><i id='p1' class='on'></i><i id='p2'></i></div></div>
<header class='hero'><h1 id='title'>连接网络</h1><p id='subtitle'>选择附近的 2.4 GHz Wi-Fi</p></header>
<div id='status' class='status' data-state='ready' role='status' aria-live='polite'>)HTML");
  html += htmlEscape(statusMessage_);
  html += F(R"HTML(</div>
<div id='connection' class='connection' data-state='ready' role='status' aria-live='polite'><div class='connection-copy'><div class='connection-title'>设备网络状态</div><div id='connection-detail' class='connection-detail'>等待选择 Wi-Fi</div></div><div id='connection-state' class='connection-state'>待设置</div></div>
<form id='form' method='post' action='/save'>
<section id='wifi-step' class='screen'>
<div class='card' aria-labelledby='wifi-title'>
<div class='card-head'><span class='number'>01</span><h2 id='wifi-title'>Wi-Fi</h2></div>
<div id='remembered-wrap' class='remembered' hidden><p class='remembered-title'>已保存网络</p><div id='remembered-list'></div></div>
<label class='field'><span class='field-name'>附近网络</span><select id='ssid' name='ssid' data-current=')HTML");
  html += htmlEscape(candidate_.wifiSsid);
  html += F(R"HTML('><option value=''>正在读取附近网络…</option></select></label>
<div class='scan-row'><span class='scan-help'>按信号强度排列</span><button id='rescan' class='secondary' type='button'>重新扫描</button></div>
<details class='disclosure manual'><summary>手工输入网络名称</summary><div class='disclosure-body'><label class='field'><span class='field-name'>网络名称</span><input id='manual' name='ssidManual' maxlength='32' autocomplete='off' autocapitalize='none' spellcheck='false' placeholder='Wi-Fi 名称'></label></div></details>
<label class='field'><span class='field-name'>Wi-Fi 密码</span><input id='wifi-password' name='wifiPassword' type='password' maxlength='63' autocomplete='current-password' autocapitalize='none' spellcheck='false' placeholder='已保存网络可留空'></label>
</div>
<div class='actions'><button id='next' class='primary' type='button'>下一步</button></div>
</section>
<section id='tencent-step' class='screen' hidden>
<div class='card' aria-labelledby='tencent-title'>
<div class='card-head'><span class='number'>02</span><h2 id='tencent-title'>腾讯云转写</h2></div>)HTML");
  if (config_ != nullptr && config_->hasTencent()) {
    html += F(R"HTML(<div class='saved-row'><span class='saved'>✓ 密钥已保存</span><span class='saved-copy'>可以直接保存</span></div>
<details id='credential-editor' class='disclosure'><summary>更换腾讯云密钥</summary><div class='disclosure-body'>)HTML");
  } else {
    html += F("<div id='credential-editor' class='credential-fields'>");
  }
  html += F(R"HTML(<label class='field'><span class='field-name'>SecretId</span><input name='secretId' maxlength='128' autocomplete='off' autocapitalize='none' spellcheck='false' placeholder='腾讯云 SecretId'></label>
<label class='field'><span class='field-name'>SecretKey</span><input name='secretKey' type='password' maxlength='128' autocomplete='new-password' autocapitalize='none' spellcheck='false' placeholder='腾讯云 SecretKey'></label>
</div>)HTML");
  if (config_ != nullptr && config_->hasTencent()) {
    html += F("</details>");
  }
  html += F(R"HTML(<details class='disclosure'><summary>高级设置</summary><div class='disclosure-body'>
<label class='field'><span class='field-name'>热词 ID <span class='optional'>可选</span></span><input name='hotwordId' maxlength='128' autocomplete='off' autocapitalize='none' spellcheck='false' value=')HTML");
  html += htmlEscape(candidate_.hotwordId);
  html += F("'></label>");
  if (config_ != nullptr && config_->hasTencent()) {
    html += F(R"HTML(<div class='danger'><label class='check'><input type='checkbox' name='clearTencent'><span>清除设备上保存的腾讯密钥</span></label></div>)HTML");
  }
  html += F(R"HTML(</div></details>
<p class='privacy'>SecretKey 保存后不会显示，也不会通过 USB 或日志读回。更新或清除密钥时，需按设备实体键确认。</p>
</div>
<div class='actions two'><button id='back' class='back' type='button'>上一步</button><button id='save' class='primary' type='submit'>保存并连接</button></div>
</section>
<section id='success-step' class='screen' hidden><div class='card result'><div><div class='result-mark'>✓</div><h2>设备已配置</h2><p>Wi-Fi 已验证，设置已安全保存。<br>热点即将自动关闭。</p></div></div></section>
</form>
<p class='footer'>热点 5 分钟后自动关闭</p>
</main>
<script>
const csrf=')HTML");
  html += csrf_.token().c_str();
  html += F(R"HTML(';
const s=document.getElementById('ssid'),b=document.getElementById('rescan'),form=document.getElementById('form'),save=document.getElementById('save'),status=document.getElementById('status'),connection=document.getElementById('connection'),connectionDetail=document.getElementById('connection-detail'),connectionState=document.getElementById('connection-state'),wifi=document.getElementById('wifi-step'),tencent=document.getElementById('tencent-step'),success=document.getElementById('success-step'),title=document.getElementById('title'),subtitle=document.getElementById('subtitle'),p1=document.getElementById('p1'),p2=document.getElementById('p2'),rememberedWrap=document.getElementById('remembered-wrap'),rememberedList=document.getElementById('remembered-list');
let preferred=s.dataset.current;
let validationTimer=0;
function strength(r){return r>=-55?'强':r>=-70?'中':'弱'}
function renderStatus(d){const state=d.wifiState||((d.saved)?'connected':(d.confirmationRequired||d.validating?'connecting':(d.scanning?'scanning':'ready')));const names={ready:['待设置','等待选择 Wi-Fi'],scanning:['扫描中','正在查找附近网络'],connecting:['连接中','正在验证 '+(d.wifiSsid||'目标网络')],connected:['已连接',d.wifiSsid?'已连接 '+d.wifiSsid:'Wi-Fi 已验证'],error:['连接失败','请检查热点后重试']};const copy=names[state]||names.ready;connection.dataset.state=state;connectionState.textContent=d.confirmationRequired?'待设备确认':copy[0];connectionDetail.textContent=d.confirmationRequired?'请按 PokePod 实体键确认腾讯密钥操作':copy[1];status.dataset.state=state;if(d.message)status.textContent=d.message}
function syncViewport(){const viewport=window.visualViewport;const height=viewport?viewport.height:window.innerHeight;document.documentElement.style.setProperty('--viewport-height',Math.round(height)+'px')}
function resetScroll(){requestAnimationFrame(()=>{const root=document.scrollingElement||document.documentElement;root.scrollTop=0;document.documentElement.scrollTop=0;document.body.scrollTop=0;requestAnimationFrame(()=>{root.scrollTop=0})})}
function renderRemembered(items){rememberedList.textContent='';rememberedWrap.hidden=!items.length;for(const item of items){const row=document.createElement('div');row.className='remembered-item';const name=document.createElement('span');name.textContent=item.ssid;const forget=document.createElement('button');forget.type='button';forget.className='forget';forget.textContent='忘记';forget.addEventListener('click',()=>forgetNetwork(item.ssid));row.append(name,forget);rememberedList.appendChild(row)}}
function render(d){const remembered=d.remembered||[];const rememberedNames=new Set(remembered.map(n=>n.ssid));renderRemembered(remembered);const chosen=s.value||preferred;s.textContent='';for(const n of d.networks){const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' · '+strength(n.rssi)+(rememberedNames.has(n.ssid)?' · 已保存':n.secured?' · 加密':' · 开放');s.appendChild(o)}if(chosen&&![...s.options].some(o=>o.value===chosen)){const o=document.createElement('option');o.value=chosen;o.textContent=chosen+' · 已保存';s.prepend(o)}if(!s.options.length){const o=document.createElement('option');o.value='';o.textContent=d.scanning?'正在扫描…':'没有发现网络';s.appendChild(o)}if([...s.options].some(o=>o.value===chosen))s.value=chosen;renderStatus(d);b.textContent=d.scanning?'扫描中…':'重新扫描';b.disabled=d.scanning;if(d.scanning)setTimeout(()=>load(false),800)}
async function load(rescan){try{const r=await fetch(rescan?'/scan':'/networks',{method:rescan?'POST':'GET',headers:rescan?{'X-PokePod-CSRF':csrf}:{},cache:'no-store'});render(await r.json())}catch(e){b.textContent='重新扫描';b.disabled=false}}
async function forgetNetwork(ssid){if(!confirm('忘记“'+ssid+'”？'))return;const body=new FormData();body.append('ssid',ssid);try{const r=await fetch('/forget-network',{method:'POST',headers:{'X-PokePod-CSRF':csrf},body,cache:'no-store'});const d=await r.json();if(!r.ok){status.textContent=d.error||'无法忘记网络';return}if(preferred===ssid)preferred='';render(d)}catch(e){status.textContent='操作失败，请保持连接 PokePod 热点'}}
function blurKeyboard(){const active=document.activeElement;if(active&&active.blur)active.blur()}
function showTencent(){const manual=document.getElementById('manual').value;if(!s.value&&!manual){status.textContent='请选择网络或手工输入名称';try{s.focus({preventScroll:true})}catch(e){s.focus()}return}blurKeyboard();setTimeout(()=>{wifi.hidden=true;tencent.hidden=false;title.textContent='腾讯云转写';subtitle.textContent='保存语音转写凭证';p1.classList.remove('on');p2.classList.add('on');syncViewport();resetScroll()},180)}
function showWifi(){blurKeyboard();tencent.hidden=true;wifi.hidden=false;title.textContent='连接网络';subtitle.textContent='选择附近的 2.4 GHz Wi-Fi';p2.classList.remove('on');p1.classList.add('on');syncViewport();resetScroll()}
function showSuccess(){clearTimeout(validationTimer);wifi.hidden=true;tencent.hidden=true;success.hidden=false;renderStatus({wifiState:'connected',saved:true,wifiSsid:s.value||preferred,message:'保存成功；热点即将关闭'});title.textContent='设置完成';subtitle.textContent='PokePod 已连接到网络';save.disabled=true;syncViewport();resetScroll()}
async function pollValidation(){try{const r=await fetch('/networks',{cache:'no-store'});const d=await r.json();renderStatus(d);if(d.saved){showSuccess();return}if(d.confirmationRequired){save.disabled=true;save.textContent='等待设备确认…';document.getElementById('back').disabled=true;validationTimer=setTimeout(pollValidation,500);return}if(d.validating){validationTimer=setTimeout(pollValidation,500);return}save.disabled=false;save.textContent='重新保存';document.getElementById('back').disabled=false}catch(e){renderStatus({wifiState:'connecting',message:'设备正在切换网络，请等待配网页恢复'});validationTimer=setTimeout(pollValidation,700)}}
async function submitForm(event){event.preventDefault();blurKeyboard();save.disabled=true;document.getElementById('back').disabled=true;save.textContent='正在连接…';renderStatus({wifiState:'connecting',message:'正在连接并验证 Wi-Fi',wifiSsid:s.value||document.getElementById('manual').value});try{const r=await fetch('/save',{method:'POST',headers:{'X-PokePod-CSRF':csrf},body:new FormData(form),cache:'no-store'});const d=await r.json();renderStatus(d);if(!r.ok){save.disabled=false;document.getElementById('back').disabled=false;save.textContent='保存并连接';return}pollValidation()}catch(e){save.disabled=false;document.getElementById('back').disabled=false;save.textContent='重新保存';renderStatus({wifiState:'connecting',message:'设备正在切换网络，请等待配网页恢复'})}}
b.addEventListener('click',()=>load(true));
document.getElementById('next').addEventListener('click',showTencent);
document.getElementById('back').addEventListener('click',showWifi);
form.addEventListener('submit',submitForm);
window.addEventListener('resize',syncViewport);
if(window.visualViewport){window.visualViewport.addEventListener('resize',syncViewport)}
syncViewport();
// The document is already on the phone before this request runs, so the
// single radio can scan without delaying the captive portal's first paint.
load(true);
</script>
</body>
</html>)HTML");
  return html;
}

String ProvisioningPortal::networksJson() const {
  String json;
  json.reserve(2048);
  json += "{\"scanning\":";
  json += scanning_ ? "true" : "false";
  json += ",\"validating\":";
  json += validating_ ? "true" : "false";
  json += ",\"saved\":";
  json += saved_ ? "true" : "false";
  json += ",\"confirmationRequired\":";
  json += sensitiveConfirmation_.pending() ? "true" : "false";
  json += ",\"confirmationAction\":\"";
  json += provisioningSensitiveActionName(sensitiveConfirmation_.action());
  json += "\",\"confirmationRemainingSeconds\":";
  json += String((sensitiveConfirmation_.remainingMs(millis()) + 999U) / 1000U);
  json += ",\"wifiState\":\"";
  json += portalState();
  json += "\",\"wifiSsid\":\"";
  json += jsonEscape(candidate_.wifiSsid);
  json += "\"";
  json += ",\"message\":\"" + jsonEscape(statusMessage_) + "\",\"networks\":[";
  for (size_t index = 0; index < networks_.size(); ++index) {
    if (index != 0) json += ',';
    json += "{\"ssid\":\"" + jsonEscape(networks_[index].ssid) + "\",\"rssi\":" +
        String(networks_[index].rssi) + ",\"secured\":" +
        (networks_[index].secured ? "true" : "false") + "}";
  }
  json += "],\"remembered\":[";
  if (config_ != nullptr) {
    const auto &remembered = config_->wifiNetworks();
    for (size_t index = 0; index < remembered.size(); ++index) {
      if (index != 0) json += ',';
      json += "{\"ssid\":\"" + jsonEscape(remembered[index].ssid) + "\"}";
    }
  }
  json += "]}";
  return json;
}

String ProvisioningPortal::htmlEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 16);
  for (size_t index = 0; index < value.length(); ++index) {
    switch (value[index]) {
      case '&': escaped += "&amp;"; break;
      case '<': escaped += "&lt;"; break;
      case '>': escaped += "&gt;"; break;
      case '"': escaped += "&quot;"; break;
      case '\'': escaped += "&#39;"; break;
      default: escaped += value[index]; break;
    }
  }
  return escaped;
}

String ProvisioningPortal::jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t index = 0; index < value.length(); ++index) {
    const unsigned char character = static_cast<unsigned char>(value[index]);
    if (character == '"' || character == '\\') {
      escaped += '\\';
      escaped += static_cast<char>(character);
    } else if (character < 0x20) {
      char encoded[7];
      snprintf(encoded, sizeof(encoded), "\\u%04x", character);
      escaped += encoded;
    } else {
      escaped += static_cast<char>(character);
    }
  }
  return escaped;
}

}  // namespace pokepod
