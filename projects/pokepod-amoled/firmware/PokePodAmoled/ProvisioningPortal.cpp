#include "ProvisioningPortal.h"

#include <algorithm>
#include <WiFi.h>
#include <esp_system.h>

namespace pokepod {
namespace {

constexpr uint32_t kPortalLifetimeMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kValidationTimeoutMs = 15000;
constexpr uint32_t kScanTimeoutMs = 8000;
constexpr size_t kMaximumNetworks = 20;

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
  statusMessage_ = "正在扫描附近的 2.4 GHz 网络";
  validating_ = false;
  scanning_ = false;
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
  startScan();
  log.printf("{\"event\":\"provisioning\",\"ok\":true,\"ssid\":\"%s\",\"expires_ms\":%lu}\n",
             ssid_.c_str(), static_cast<unsigned long>(kPortalLifetimeMs));
  return true;
}

void ProvisioningPortal::loop(uint32_t nowMs) {
  if (!active_) return;
  dns_.processNextRequest();
  server_.handleClient();
  pollScan();
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
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
  active_ = false;
  validating_ = false;
  scanning_ = false;
  networks_.clear();
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
  server_.on("/networks", HTTP_GET, [this]() { showNetworks(); });
  server_.on("/scan", HTTP_POST, [this]() { scanRequest(); });
  server_.on("/save", HTTP_POST, [this]() { saveRequest(); });
  server_.on("/status", HTTP_GET, [this]() { showPortal(); });
  server_.on("/generate_204", HTTP_ANY, [this]() { redirectPortal(); });
  server_.on("/hotspot-detect.html", HTTP_ANY, [this]() { redirectPortal(); });
  server_.on("/connecttest.txt", HTTP_ANY, [this]() { redirectPortal(); });
  server_.onNotFound([this]() { redirectPortal(); });
  routesInstalled_ = true;
}

void ProvisioningPortal::startScan() {
  if (!active_ || validating_ || scanning_) return;
  const int16_t result = WiFi.scanNetworks(true, false, false, 120);
  scanning_ = result == WIFI_SCAN_RUNNING;
  scanStartedMs_ = millis();
  if (!scanning_) {
    statusMessage_ = "扫描启动失败；可手工输入网络名称";
    if (log_ != nullptr) {
      log_->println("{\"event\":\"wifi_scan\",\"ok\":false,\"stage\":\"start\"}");
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
    statusMessage_ = "扫描超时；可重新扫描或手工输入";
    if (log_ != nullptr) {
      log_->println("{\"event\":\"wifi_scan\",\"ok\":false,\"stage\":\"timeout\"}");
    }
    return;
  }
  if (count < 0) {
    scanning_ = false;
    WiFi.scanDelete();
    statusMessage_ = "扫描失败；可重新扫描或手工输入";
    if (log_ != nullptr) {
      log_->println("{\"event\":\"wifi_scan\",\"ok\":false,\"stage\":\"complete\"}");
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
  statusMessage_ = networks_.empty()
      ? "没有发现网络；可重新扫描或手工输入"
      : "请选择附近的 2.4 GHz 网络";
  if (log_ != nullptr) {
    log_->printf("{\"event\":\"wifi_scan\",\"ok\":true,\"networks\":%u}\n",
                 static_cast<unsigned>(networks_.size()));
  }
}

void ProvisioningPortal::showNetworks() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json; charset=utf-8", networksJson());
}

void ProvisioningPortal::scanRequest() {
  if (validating_) {
    server_.send(409, "application/json; charset=utf-8",
                 "{\"error\":\"正在验证 Wi-Fi\"}");
    return;
  }
  startScan();
  showNetworks();
}

void ProvisioningPortal::showPortal() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "text/html; charset=utf-8", pageHtml());
}

void ProvisioningPortal::saveRequest() {
  if (validating_) {
    statusMessage_ = "正在验证 Wi-Fi，请稍候";
    showPortal();
    return;
  }
  if (scanning_) {
    statusMessage_ = "正在扫描附近网络，请稍候";
    showPortal();
    return;
  }
  DeviceSettings next = config_->settings();
  const String manualSsid = server_.arg("ssidManual");
  next.wifiSsid = manualSsid.isEmpty() ? server_.arg("ssid") : manualSsid;
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
  html.reserve(7000);
  html += F("<!doctype html><html lang='zh-CN'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>PokePod 配网</title><style>body{font-family:-apple-system,sans-serif;background:#0b0c10;color:#fff;margin:0;padding:24px}.card{max-width:520px;margin:auto;background:#171922;border-radius:20px;padding:22px}h1{margin-top:0}label{display:block;margin:16px 0 6px;color:#b9c0d0}input,select{box-sizing:border-box;width:100%;padding:13px;border-radius:10px;border:1px solid #343848;background:#0d0f15;color:#fff}.primary{width:100%;margin-top:22px;padding:14px;border:0;border-radius:12px;background:#1976ff;color:white;font-size:17px}.secondary{width:auto;margin-top:10px;padding:9px 12px;border:1px solid #4b5267;border-radius:9px;background:#252a38;color:#fff}.status{padding:12px;border-radius:10px;background:#10294a}.hint{font-size:13px;color:#a9b0c0;line-height:1.55}.row{display:flex;gap:8px;align-items:center}.row input{width:auto}</style><body><div class='card'><h1>PokePod 配网</h1><div id='status' class='status'>");
  html += htmlEscape(statusMessage_);
  html += F("</div><form method='post' action='/save'><label>附近的 2.4 GHz Wi-Fi</label><select id='ssid' name='ssid' data-current='");
  html += htmlEscape(candidate_.wifiSsid);
  html += F("'><option value=''>正在读取附近网络…</option></select><button id='rescan' class='secondary' type='button'>重新扫描</button><label>隐藏网络或手工输入（可选）</label><input name='ssidManual' maxlength='32' placeholder='选择列表时留空'><label>Wi-Fi 密码</label><input name='wifiPassword' type='password' maxlength='63' placeholder='请重新输入'><label>腾讯 SecretId</label><input name='secretId' autocomplete='off' placeholder='留空则保留已有值'><label>腾讯 SecretKey</label><input name='secretKey' type='password' autocomplete='off' placeholder='留空则保留已有值'><label>热词 ID（可选）</label><input name='hotwordId' maxlength='128' value='");
  html += htmlEscape(candidate_.hotwordId);
  html += F("'><label class='row'><input type='checkbox' name='clearTencent'>清除已保存的腾讯密钥</label><button class='primary' type='submit'>保存并验证 Wi-Fi</button></form><p class='hint'>PokePod 只支持 2.4 GHz。已保存的 SecretKey 不会显示在本页，也不会通过 USB 状态或日志读回。腾讯权限会在第一条胶囊转写时验证。</p></div><script>const s=document.getElementById('ssid'),b=document.getElementById('rescan'),status=document.getElementById('status');let preferred=s.dataset.current;function strength(r){return r>=-55?'强':r>=-70?'中':'弱'}function render(d){const chosen=s.value||preferred;s.textContent='';for(const n of d.networks){const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' · '+strength(n.rssi)+(n.secured?' · 加密':' · 开放');s.appendChild(o)}if(chosen&&![...s.options].some(o=>o.value===chosen)){const o=document.createElement('option');o.value=chosen;o.textContent=chosen+' · 已保存';s.prepend(o)}if(!s.options.length){const o=document.createElement('option');o.value='';o.textContent=d.scanning?'正在扫描…':'没有发现网络';s.appendChild(o)}if([...s.options].some(o=>o.value===chosen))s.value=chosen;if(d.message)status.textContent=d.message;b.textContent=d.scanning?'扫描中…':'重新扫描';b.disabled=d.scanning;if(d.scanning)setTimeout(()=>load(false),800)}async function load(rescan){try{const r=await fetch(rescan?'/scan':'/networks',{method:rescan?'POST':'GET',cache:'no-store'});render(await r.json())}catch(e){b.textContent='重新扫描';b.disabled=false}}b.addEventListener('click',()=>load(true));load(false)</script></body></html>");
  return html;
}

String ProvisioningPortal::networksJson() const {
  String json;
  json.reserve(2048);
  json += "{\"scanning\":";
  json += scanning_ ? "true" : "false";
  json += ",\"message\":\"" + jsonEscape(statusMessage_) + "\",\"networks\":[";
  for (size_t index = 0; index < networks_.size(); ++index) {
    if (index != 0) json += ',';
    json += "{\"ssid\":\"" + jsonEscape(networks_[index].ssid) + "\",\"rssi\":" +
        String(networks_[index].rssi) + ",\"secured\":" +
        (networks_[index].secured ? "true" : "false") + "}";
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
