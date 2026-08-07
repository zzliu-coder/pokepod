#include "ProvisioningPortal.h"

#include <algorithm>
#include <WiFi.h>

#include "ProvisioningPolicy.h"

namespace pokepod {
namespace {

constexpr uint32_t kPortalLifetimeMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kValidationTimeoutMs = 15000;
constexpr uint32_t kScanTimeoutMs = 8000;
constexpr size_t kMaximumNetworks = 20;

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
  password_ = kProvisioningPassword;
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
  html.reserve(12000);
  html += F(R"HTML(<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'><meta name='theme-color' content='#000000'><title>PokePod 设置</title><style>
*{box-sizing:border-box}html{min-height:100%;overflow-x:hidden;overflow-y:auto;background:#000;-webkit-text-size-adjust:100%}body{min-height:100%;margin:0;overflow-x:hidden;overflow-y:visible;background:#000;color:#f4faf7;font-family:-apple-system,BlinkMacSystemFont,"SF Pro Text","PingFang SC",sans-serif;-webkit-overflow-scrolling:touch;touch-action:pan-y}button,input,select{font:inherit}.shell{width:100%;max-width:560px;min-height:100vh;min-height:100dvh;margin:0 auto;padding:calc(28px + env(safe-area-inset-top)) 18px calc(56px + env(safe-area-inset-bottom))}.brand{display:flex;align-items:center;gap:11px;color:#8ea39c;font-size:14px;font-weight:650;letter-spacing:.04em}.mark{position:relative;width:31px;height:20px;border:2px solid #69e0b6;border-radius:999px;transform:rotate(-9deg)}.mark:after{content:"";position:absolute;inset:4px 7px;border:1px solid #69e0b6;border-radius:999px;opacity:.55}.hero{padding:26px 4px 20px}.eyebrow{margin:0 0 7px;color:#69e0b6;font-size:13px;font-weight:750;letter-spacing:.12em}.hero h1{margin:0;font-size:32px;line-height:1.12;letter-spacing:-.04em}.hero p{margin:11px 0 0;color:#8ea39c;font-size:15px;line-height:1.6}.status{display:flex;align-items:flex-start;gap:10px;margin:0 0 16px;padding:13px 15px;border:1px solid #1a2a25;border-radius:14px;background:#0b1311;color:#c9d8d2;font-size:14px;line-height:1.5}.status:before{content:"";flex:0 0 auto;width:8px;height:8px;margin-top:6px;border-radius:50%;background:#f0c45b;box-shadow:0 0 16px rgba(240,196,91,.55)}.section{margin-top:14px;padding:20px 17px 18px;border:1px solid #1a2a25;border-radius:20px;background:#0b1311}.section-head{display:grid;grid-template-columns:36px 1fr;column-gap:12px;align-items:center;margin-bottom:18px}.step{display:grid;place-items:center;width:36px;height:36px;border-radius:12px;background:#12372c;color:#69e0b6;font-size:14px;font-weight:800}.section h2{margin:0;font-size:20px;line-height:1.2;letter-spacing:-.02em}.section-note{grid-column:2;margin:5px 0 0;color:#8ea39c;font-size:13px;line-height:1.45}.saved{grid-column:2;justify-self:start;display:inline-flex;align-items:center;gap:6px;margin-top:8px;padding:5px 9px;border-radius:999px;background:#12372c;color:#69e0b6;font-size:12px;font-weight:700}.field{display:block;margin-top:15px}.field:first-of-type{margin-top:0}.field-name{display:block;margin-bottom:7px;color:#c9d8d2;font-size:14px;font-weight:650}.optional{color:#8ea39c;font-weight:500}input,select{display:block;width:100%;min-height:50px;padding:13px 14px;border:1px solid #263b34;border-radius:13px;outline:0;background:#050908;color:#f4faf7;font-size:16px;line-height:1.35;scroll-margin:28vh 0}input::placeholder{color:#657a72}input:focus,select:focus{border-color:#69e0b6;box-shadow:0 0 0 3px rgba(105,224,182,.12)}select{appearance:none;background-image:linear-gradient(45deg,transparent 50%,#8ea39c 50%),linear-gradient(135deg,#8ea39c 50%,transparent 50%);background-position:calc(100% - 19px) 21px,calc(100% - 14px) 21px;background-size:5px 5px;background-repeat:no-repeat;padding-right:40px}.scan-row{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-top:10px}.scan-help{color:#8ea39c;font-size:12px;line-height:1.4}.secondary{flex:0 0 auto;min-height:40px;padding:9px 13px;border:1px solid #2c4a40;border-radius:12px;background:#101a17;color:#69e0b6;font-size:14px;font-weight:700}.secondary:disabled{opacity:.45}.manual{margin-top:13px;border-top:1px solid #1a2a25;padding-top:12px}.manual summary{cursor:pointer;color:#8ea39c;font-size:14px;list-style:none}.manual summary::-webkit-details-marker{display:none}.manual summary:after{content:"＋";float:right;color:#69e0b6}.manual[open] summary:after{content:"－"}.danger{margin-top:15px;padding-top:13px;border-top:1px solid #1a2a25}.check{display:flex;align-items:flex-start;gap:10px;color:#8ea39c;font-size:13px;line-height:1.45}.check input{width:20px;min-height:20px;height:20px;margin:0;accent-color:#ff786d}.primary{width:100%;min-height:54px;margin-top:18px;border:0;border-radius:16px;background:#69e0b6;color:#07110d;font-size:17px;font-weight:800;box-shadow:0 10px 34px rgba(105,224,182,.14)}.primary:active{transform:scale(.99)}.privacy{margin:17px 5px 0;color:#8ea39c;font-size:12px;line-height:1.65}.privacy strong{color:#c9d8d2}.footer{margin:26px 0 0;text-align:center;color:#52655e;font-size:12px}@media(min-width:600px){.shell{padding-left:24px;padding-right:24px}.section{padding:24px}.hero{padding-top:34px}}@media(prefers-reduced-motion:no-preference){.mark{animation:breathe 2.8s ease-in-out infinite}@keyframes breathe{50%{transform:rotate(-9deg) scale(1.08);filter:drop-shadow(0 0 7px rgba(105,224,182,.32))}}}
</style></head><body><main class='shell'><div class='brand'><span class='mark' aria-hidden='true'></span><span>POKEPOD</span></div><header class='hero'><p class='eyebrow'>设备设置</p><h1>让胶囊开始工作</h1><p>先让 PokePod 连上网络，再配置腾讯云转写。内容只会保存到这台设备。</p></header><div id='status' class='status' role='status' aria-live='polite'>)HTML");
  html += htmlEscape(statusMessage_);
  html += F(R"HTML(</div><form method='post' action='/save'><section class='section' aria-labelledby='wifi-title'><div class='section-head'><span class='step'>01</span><h2 id='wifi-title'>连接网络</h2><p class='section-note'>仅支持 2.4 GHz Wi-Fi</p></div><label class='field'><span class='field-name'>附近网络</span><select id='ssid' name='ssid' data-current=')HTML");
  html += htmlEscape(candidate_.wifiSsid);
  html += F(R"HTML('><option value=''>正在读取附近网络…</option></select></label><div class='scan-row'><span class='scan-help'>按信号强度排列</span><button id='rescan' class='secondary' type='button'>重新扫描</button></div><details class='manual'><summary>找不到网络？手动输入</summary><label class='field'><span class='field-name'>网络名称</span><input name='ssidManual' maxlength='32' autocomplete='off' autocapitalize='none' spellcheck='false' placeholder='输入 Wi-Fi 名称'></label></details><label class='field'><span class='field-name'>Wi-Fi 密码</span><input name='wifiPassword' type='password' maxlength='63' autocomplete='current-password' autocapitalize='none' spellcheck='false' placeholder='输入网络密码'></label></section><section class='section' aria-labelledby='tencent-title'><div class='section-head'><span class='step'>02</span><h2 id='tencent-title'>腾讯云转写</h2><p class='section-note'>语音胶囊会自动转成文字</p>)HTML");
  if (config_ != nullptr && config_->hasTencent()) {
    html += F("<span class='saved'>✓ 已保存密钥</span>");
  }
  html += F(R"HTML(</div><label class='field'><span class='field-name'>SecretId</span><input name='secretId' maxlength='128' autocomplete='off' autocapitalize='none' spellcheck='false' placeholder='留空会保留已有值'></label><label class='field'><span class='field-name'>SecretKey</span><input name='secretKey' type='password' maxlength='128' autocomplete='new-password' autocapitalize='none' spellcheck='false' placeholder='留空会保留已有值'></label><label class='field'><span class='field-name'>热词 ID <span class='optional'>可选</span></span><input name='hotwordId' maxlength='128' autocomplete='off' autocapitalize='none' spellcheck='false' value=')HTML");
  html += htmlEscape(candidate_.hotwordId);
  html += F(R"HTML('></label><div class='danger'><label class='check'><input type='checkbox' name='clearTencent'><span>清除这台设备上已保存的腾讯密钥</span></label></div></section><button class='primary' type='submit'>保存并连接</button></form><p class='privacy'><strong>密钥保护：</strong>已保存的 SecretKey 不会显示，也不会通过 USB 或日志读回。腾讯权限会在第一条胶囊转写时验证。</p><p class='footer'>热点将在 5 分钟后自动关闭</p></main><script>
const s=document.getElementById('ssid'),b=document.getElementById('rescan'),status=document.getElementById('status');let preferred=s.dataset.current;function strength(r){return r>=-55?'强':r>=-70?'中':'弱'}function render(d){const chosen=s.value||preferred;s.textContent='';for(const n of d.networks){const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' · '+strength(n.rssi)+(n.secured?' · 加密':' · 开放');s.appendChild(o)}if(chosen&&![...s.options].some(o=>o.value===chosen)){const o=document.createElement('option');o.value=chosen;o.textContent=chosen+' · 已保存';s.prepend(o)}if(!s.options.length){const o=document.createElement('option');o.value='';o.textContent=d.scanning?'正在扫描…':'没有发现网络';s.appendChild(o)}if([...s.options].some(o=>o.value===chosen))s.value=chosen;if(d.message)status.textContent=d.message;b.textContent=d.scanning?'扫描中…':'重新扫描';b.disabled=d.scanning;if(d.scanning)setTimeout(()=>load(false),800)}async function load(rescan){try{const r=await fetch(rescan?'/scan':'/networks',{method:rescan?'POST':'GET',cache:'no-store'});render(await r.json())}catch(e){b.textContent='重新扫描';b.disabled=false}}b.addEventListener('click',()=>load(true));for(const field of document.querySelectorAll('input,select'))field.addEventListener('focus',()=>setTimeout(()=>field.scrollIntoView({block:'center',behavior:'smooth'}),260));load(false);
</script></body></html>)HTML");
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
