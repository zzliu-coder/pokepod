#include "ProvisioningPortal.h"

#include <WiFi.h>
#include <esp_system.h>

namespace pokepod {
namespace {

constexpr uint32_t kPortalLifetimeMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kValidationTimeoutMs = 15000;

String randomPassword() {
  static constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  String value;
  value.reserve(12);
  for (uint8_t index = 0; index < 12; ++index) {
    value += alphabet[esp_random() % (sizeof(alphabet) - 1)];
  }
  return value;
}

}  // namespace

ProvisioningPortal::ProvisioningPortal() : server_(80) {}

bool ProvisioningPortal::begin(DeviceConfig &config, Print &log) {
  if (active_) return true;
  config_ = &config;
  log_ = &log;
  candidate_ = config.settings();
  const uint32_t suffix = static_cast<uint32_t>(ESP.getEfuseMac() & 0xffff);
  char name[24];
  snprintf(name, sizeof(name), "PokePod-%04lX", static_cast<unsigned long>(suffix));
  ssid_ = name;
  password_ = randomPassword();
  statusMessage_ = "请输入配置";
  validating_ = false;
  changed_ = false;
  closeAtMs_ = 0;
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(ssid_.c_str(), password_.c_str())) {
    log.println("{\"event\":\"provisioning\",\"ok\":false,\"stage\":\"softap\"}");
    return false;
  }
  installRoutes();
  dns_.start(53, "*", WiFi.softAPIP());
  server_.begin();
  startedMs_ = millis();
  active_ = true;
  log.printf("{\"event\":\"provisioning\",\"ok\":true,\"ssid\":\"%s\",\"expires_ms\":%lu}\n",
             ssid_.c_str(), static_cast<unsigned long>(kPortalLifetimeMs));
  return true;
}

void ProvisioningPortal::loop(uint32_t nowMs) {
  if (!active_) return;
  dns_.processNextRequest();
  server_.handleClient();
  if (validating_) {
    if (WiFi.status() == WL_CONNECTED) {
      validating_ = false;
      if (config_->save(candidate_, *log_)) {
        changed_ = true;
        statusMessage_ = "保存成功；热点即将关闭";
        closeAtMs_ = nowMs + 1800;
      } else {
        statusMessage_ = "写入配置失败，请重试";
      }
    } else if (static_cast<uint32_t>(nowMs - validatingSinceMs_) >=
               kValidationTimeoutMs) {
      validating_ = false;
      WiFi.disconnect(false, false);
      statusMessage_ = "无法连接该 Wi-Fi，请检查名称和密码";
    }
  }
  if ((closeAtMs_ != 0 && static_cast<int32_t>(nowMs - closeAtMs_) >= 0) ||
      static_cast<uint32_t>(nowMs - startedMs_) >= kPortalLifetimeMs) {
    stop();
  }
}

void ProvisioningPortal::stop() {
  if (!active_) return;
  dns_.stop();
  server_.stop();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);
  active_ = false;
  validating_ = false;
  closeAtMs_ = 0;
  if (log_ != nullptr) log_->println("{\"event\":\"provisioning_stopped\"}");
}

bool ProvisioningPortal::takeConfigurationChanged() {
  const bool value = changed_;
  changed_ = false;
  return value;
}

void ProvisioningPortal::installRoutes() {
  if (routesInstalled_) return;
  server_.on("/", HTTP_GET, [this]() { showPortal(); });
  server_.on("/save", HTTP_POST, [this]() { saveRequest(); });
  server_.on("/status", HTTP_GET, [this]() { showPortal(); });
  server_.on("/generate_204", HTTP_ANY, [this]() { redirectPortal(); });
  server_.on("/hotspot-detect.html", HTTP_ANY, [this]() { redirectPortal(); });
  server_.on("/connecttest.txt", HTTP_ANY, [this]() { redirectPortal(); });
  server_.onNotFound([this]() { redirectPortal(); });
  routesInstalled_ = true;
}

void ProvisioningPortal::showPortal() {
  server_.send(200, "text/html; charset=utf-8", pageHtml());
}

void ProvisioningPortal::saveRequest() {
  if (validating_) {
    statusMessage_ = "正在验证 Wi-Fi，请稍候";
    showPortal();
    return;
  }
  DeviceSettings next = config_->settings();
  next.wifiSsid = server_.arg("ssid");
  next.wifiPassword = server_.arg("wifiPassword");
  next.hotwordId = server_.arg("hotwordId");
  next.wifiEnabled = true;
  const String secretId = server_.arg("secretId");
  const String secretKey = server_.arg("secretKey");
  if (server_.hasArg("clearTencent")) {
    next.secretId = "";
    next.secretKey = "";
    next.hotwordId = "";
  } else if (!secretId.isEmpty() || !secretKey.isEmpty()) {
    if (secretId.isEmpty() || secretKey.isEmpty()) {
      statusMessage_ = "SecretId 和 SecretKey 必须同时填写";
      showPortal();
      return;
    }
    next.secretId = secretId;
    next.secretKey = secretKey;
  }
  if (next.wifiSsid.isEmpty() || next.wifiSsid.length() > 32 ||
      (!next.wifiPassword.isEmpty() &&
       (next.wifiPassword.length() < 8 || next.wifiPassword.length() > 63))) {
    statusMessage_ = "Wi-Fi 名称或密码长度不正确";
    showPortal();
    return;
  }
  candidate_ = next;
  statusMessage_ = "正在连接并验证 Wi-Fi";
  validating_ = true;
  validatingSinceMs_ = millis();
  WiFi.begin(candidate_.wifiSsid.c_str(), candidate_.wifiPassword.c_str());
  showPortal();
}

void ProvisioningPortal::redirectPortal() {
  server_.sendHeader("Location", "http://192.168.4.1/", true);
  server_.send(302, "text/plain", "");
}

String ProvisioningPortal::pageHtml() const {
  String html;
  html.reserve(4200);
  html += F("<!doctype html><html lang='zh-CN'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>PokePod 配网</title><style>body{font-family:-apple-system,sans-serif;background:#0b0c10;color:#fff;margin:0;padding:24px}.card{max-width:520px;margin:auto;background:#171922;border-radius:20px;padding:22px}h1{margin-top:0}label{display:block;margin:16px 0 6px;color:#b9c0d0}input{box-sizing:border-box;width:100%;padding:13px;border-radius:10px;border:1px solid #343848;background:#0d0f15;color:#fff}button{width:100%;margin-top:22px;padding:14px;border:0;border-radius:12px;background:#1976ff;color:white;font-size:17px}.status{padding:12px;border-radius:10px;background:#10294a}.hint{font-size:13px;color:#a9b0c0;line-height:1.55}.row{display:flex;gap:8px;align-items:center}.row input{width:auto}</style><body><div class='card'><h1>PokePod 配网</h1><div class='status'>");
  html += htmlEscape(statusMessage_);
  html += F("</div><form method='post' action='/save'><label>Wi-Fi 名称</label><input name='ssid' maxlength='32' required value='");
  html += htmlEscape(candidate_.wifiSsid);
  html += F("'><label>Wi-Fi 密码</label><input name='wifiPassword' type='password' maxlength='63' placeholder='请重新输入'><label>腾讯 SecretId</label><input name='secretId' autocomplete='off' placeholder='留空则保留已有值'><label>腾讯 SecretKey</label><input name='secretKey' type='password' autocomplete='off' placeholder='留空则保留已有值'><label>热词 ID（可选）</label><input name='hotwordId' maxlength='128' value='");
  html += htmlEscape(candidate_.hotwordId);
  html += F("'><label class='row'><input type='checkbox' name='clearTencent'>清除已保存的腾讯密钥</label><button type='submit'>保存并验证 Wi-Fi</button></form><p class='hint'>已保存的 SecretKey 不会显示在本页，也不会通过 USB 状态或日志读回。腾讯权限会在第一条胶囊转写时验证。</p></div></body></html>");
  return html;
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

}  // namespace pokepod
