#include "DeviceConfig.h"

namespace pokepod {

bool DeviceConfig::begin(Print &log) {
  open_ = preferences_.begin("pokepod", false);
  if (!open_) {
    log.println("{\"event\":\"config\",\"ok\":false,\"stage\":\"nvs_open\"}");
    return false;
  }
  settings_.wifiSsid = preferences_.getString("wifi_ssid", "");
  settings_.wifiPassword = preferences_.getString("wifi_pass", "");
  settings_.secretId = preferences_.getString("secret_id", "");
  settings_.secretKey = preferences_.getString("secret_key", "");
  settings_.hotwordId = preferences_.getString("hotword_id", "");
  settings_.wifiEnabled = preferences_.getBool("wifi_enabled", true);
  settings_.raiseToWake = preferences_.getBool("raise_wake", true);
  log.printf("{\"event\":\"config\",\"ok\":true,\"wifi_configured\":%s,\"tencent_configured\":%s}\n",
             hasWifi() ? "true" : "false", hasTencent() ? "true" : "false");
  return true;
}

bool DeviceConfig::save(const DeviceSettings &settings, Print &log) {
  if (!open_ || settings.wifiSsid.length() > 32 ||
      settings.wifiPassword.length() > 63 || settings.secretId.length() > 128 ||
      settings.secretKey.length() > 128 || settings.hotwordId.length() > 128) {
    return false;
  }
  bool ok = preferences_.putString("wifi_ssid", settings.wifiSsid) > 0;
  if (settings.wifiPassword.isEmpty()) preferences_.remove("wifi_pass");
  else ok = preferences_.putString("wifi_pass", settings.wifiPassword) > 0 && ok;
  if (settings.secretId.isEmpty()) preferences_.remove("secret_id");
  else ok = preferences_.putString("secret_id", settings.secretId) > 0 && ok;
  if (settings.secretKey.isEmpty()) preferences_.remove("secret_key");
  else ok = preferences_.putString("secret_key", settings.secretKey) > 0 && ok;
  if (settings.hotwordId.isEmpty()) preferences_.remove("hotword_id");
  else ok = preferences_.putString("hotword_id", settings.hotwordId) > 0 && ok;
  ok = preferences_.putBool("wifi_enabled", settings.wifiEnabled) > 0 && ok;
  ok = preferences_.putBool("raise_wake", settings.raiseToWake) > 0 && ok;
  if (ok) settings_ = settings;
  log.printf("{\"event\":\"config_saved\",\"ok\":%s,\"wifi_configured\":%s,\"tencent_configured\":%s}\n",
             ok ? "true" : "false", !settings.wifiSsid.isEmpty() ? "true" : "false",
             (!settings.secretId.isEmpty() && !settings.secretKey.isEmpty()) ? "true" : "false");
  return ok;
}

bool DeviceConfig::clearWifi(Print &log) {
  if (!open_) return false;
  const bool ok = preferences_.remove("wifi_ssid") |
                  preferences_.remove("wifi_pass");
  settings_.wifiSsid = "";
  settings_.wifiPassword = "";
  log.printf("{\"event\":\"wifi_credentials_cleared\",\"ok\":%s}\n",
             ok ? "true" : "false");
  return ok;
}

bool DeviceConfig::clearTencent(Print &log) {
  if (!open_) return false;
  const bool ok = preferences_.remove("secret_id") |
                  preferences_.remove("secret_key") |
                  preferences_.remove("hotword_id");
  settings_.secretId = "";
  settings_.secretKey = "";
  settings_.hotwordId = "";
  log.printf("{\"event\":\"tencent_credentials_cleared\",\"ok\":%s}\n",
             ok ? "true" : "false");
  return ok;
}

bool DeviceConfig::setWifiEnabled(bool enabled, Print &log) {
  if (!open_ || preferences_.putBool("wifi_enabled", enabled) == 0) return false;
  settings_.wifiEnabled = enabled;
  log.printf("{\"event\":\"wifi_manual\",\"enabled\":%s}\n",
             enabled ? "true" : "false");
  return true;
}

bool DeviceConfig::setRaiseToWake(bool enabled, Print &log) {
  if (!open_ || preferences_.putBool("raise_wake", enabled) == 0) return false;
  settings_.raiseToWake = enabled;
  log.printf("{\"event\":\"raise_to_wake\",\"enabled\":%s}\n",
             enabled ? "true" : "false");
  return true;
}

}  // namespace pokepod
