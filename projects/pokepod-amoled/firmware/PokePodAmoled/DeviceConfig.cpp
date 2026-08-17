#include "DeviceConfig.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "DeviceConfigBlob.h"
#include "RememberedWifiPolicy.h"

namespace pokepod {
namespace {

constexpr char kConfigBlobKey[] = "config_v1";

void copyString(char *destination, size_t capacity, const String &source) {
  const size_t sourceLength = source.length();
  const size_t length = std::min(sourceLength, capacity - 1);
  if (length > 0) std::memcpy(destination, source.c_str(), length);
  destination[length] = '\0';
}

void applyPreferredWifi(DeviceSettings &settings,
                        const std::vector<WifiCredential> &networks) {
  if (networks.empty()) {
    settings.wifiSsid = "";
    settings.wifiPassword = "";
    return;
  }
  settings.wifiSsid = networks.front().ssid;
  settings.wifiPassword = networks.front().password;
}

StoredDeviceConfig encodeConfig(
    const DeviceSettings &settings,
    const std::vector<WifiCredential> &networks) {
  StoredDeviceConfig stored{};
  stored.magic = kDeviceConfigMagic;
  stored.version = kDeviceConfigVersion;
  stored.wifiCount = static_cast<uint8_t>(networks.size());
  stored.wifiEnabled = settings.wifiEnabled ? 1 : 0;
  stored.raiseToWake = settings.raiseToWake ? 1 : 0;
  stored.reserved[0] = settings.bluetoothEnabled
      ? kStoredBluetoothEnabled : kStoredBluetoothDisabled;
  stored.reserved[1] = static_cast<uint8_t>(settings.provisioningPasswordMode);
  for (size_t index = 0; index < networks.size(); ++index) {
    copyString(stored.wifi[index].ssid, sizeof(stored.wifi[index].ssid),
               networks[index].ssid);
    copyString(stored.wifi[index].password,
               sizeof(stored.wifi[index].password), networks[index].password);
  }
  copyString(stored.secretId, sizeof(stored.secretId), settings.secretId);
  copyString(stored.secretKey, sizeof(stored.secretKey), settings.secretKey);
  copyString(stored.hotwordId, sizeof(stored.hotwordId), settings.hotwordId);
  finalizeDeviceConfigBlob(stored);
  return stored;
}

bool decodeConfig(const StoredDeviceConfig &stored, DeviceSettings &settings,
                  std::vector<WifiCredential> &networks) {
  if (!validateDeviceConfigBlob(stored)) return false;

  std::vector<WifiCredential> decodedNetworks;
  decodedNetworks.reserve(stored.wifiCount);
  for (size_t index = 0; index < stored.wifiCount; ++index) {
    const StoredWifiCredential &network = stored.wifi[index];
    const String ssid(network.ssid);
    const String password(network.password);
    if (ssid.length() > 32 || password.length() > 63 ||
        std::any_of(decodedNetworks.begin(), decodedNetworks.end(),
            [&ssid](const WifiCredential &item) { return item.ssid == ssid; })) {
      return false;
    }
    decodedNetworks.push_back({ssid, password});
  }

  DeviceSettings decodedSettings;
  decodedSettings.secretId = stored.secretId;
  decodedSettings.secretKey = stored.secretKey;
  decodedSettings.hotwordId = stored.hotwordId;
  decodedSettings.wifiEnabled = stored.wifiEnabled != 0;
  decodedSettings.bluetoothEnabled =
      storedDeviceConfigBluetoothEnabled(stored);
  decodedSettings.raiseToWake = stored.raiseToWake != 0;
  decodedSettings.provisioningPasswordMode =
      storedDeviceConfigProvisioningPasswordMode(stored);
  applyPreferredWifi(decodedSettings, decodedNetworks);
  settings = decodedSettings;
  networks = decodedNetworks;
  return true;
}

bool validSettings(const DeviceSettings &settings) {
  return validProvisioningPasswordMode(settings.provisioningPasswordMode) &&
      settings.wifiSsid.length() <= 32 &&
      settings.wifiPassword.length() <= 63 &&
      settings.secretId.length() <= 128 &&
      settings.secretKey.length() <= 128 &&
      settings.hotwordId.length() <= 128;
}

}  // namespace

bool DeviceConfig::begin(Print &log) {
  open_ = preferences_.begin("pokepod", false);
  if (!open_) {
    log.println("{\"event\":\"config\",\"ok\":false,\"stage\":\"nvs_open\"}");
    return false;
  }

  StoredDeviceConfig stored{};
  const bool hasStoredBlob =
      preferences_.getBytesLength(kConfigBlobKey) == sizeof(stored);
  if (hasStoredBlob &&
      preferences_.getBytes(kConfigBlobKey, &stored, sizeof(stored)) ==
          sizeof(stored) &&
      decodeConfig(stored, settings_, wifiNetworks_)) {
    const ProvisioningPasswordMode storedMode =
        settings_.provisioningPasswordMode;
    bool passwordModePersisted = true;
    if (storedMode == ProvisioningPasswordMode::legacy) {
      settings_.provisioningPasswordMode =
          migrateProvisioningPasswordMode(storedMode);
      passwordModePersisted = persistState(settings_, wifiNetworks_);
      log.printf(
          "{\"event\":\"provisioning_password_mode_migrated\","
          "\"ok\":%s,\"from\":0,\"to\":%u}\n",
          passwordModePersisted ? "true" : "false",
          static_cast<unsigned>(settings_.provisioningPasswordMode));
    }
    log.printf("{\"event\":\"config\",\"ok\":true,\"wifi_configured\":%s,\"wifi_network_count\":%u,\"tencent_configured\":%s,\"provisioning_password_mode\":%u,\"password_mode_persisted\":%s}\n",
               hasWifi() ? "true" : "false",
               static_cast<unsigned>(wifiNetworks_.size()),
               hasTencent() ? "true" : "false",
               static_cast<unsigned>(settings_.provisioningPasswordMode),
               passwordModePersisted ? "true" : "false");
    return true;
  }

  // One-time migration from the pre-blob layout. A valid blob, including a
  // valid empty network list, always wins and never consults these keys.
  DeviceSettings migrated;
  migrated.secretId = preferences_.getString("secret_id", "");
  migrated.secretKey = preferences_.getString("secret_key", "");
  migrated.hotwordId = preferences_.getString("hotword_id", "");
  migrated.wifiEnabled = preferences_.getBool("wifi_enabled", true);
  migrated.bluetoothEnabled = preferences_.getBool("ble_enabled", true);
  migrated.raiseToWake = preferences_.getBool("raise_wake", true);

  std::vector<WifiCredential> migratedNetworks;
  const uint8_t storedCount = std::min<uint8_t>(
      preferences_.getUChar("wifi_count", 0), kMaximumRememberedWifiNetworks);
  for (uint8_t index = 0; index < storedCount; ++index) {
    const String suffix(index);
    const String ssid = preferences_.getString(("wifi_s" + suffix).c_str(), "");
    const String password =
        preferences_.getString(("wifi_p" + suffix).c_str(), "");
    if (!ssid.isEmpty() && ssid.length() <= 32 && password.length() <= 63 &&
        std::none_of(migratedNetworks.begin(), migratedNetworks.end(),
            [&ssid](const WifiCredential &network) {
              return network.ssid == ssid;
            })) {
      migratedNetworks.push_back({ssid, password});
    }
  }
  const String legacySsid = preferences_.getString("wifi_ssid", "");
  const String legacyPassword = preferences_.getString("wifi_pass", "");
  if (!legacySsid.isEmpty() && legacySsid.length() <= 32 &&
      legacyPassword.length() <= 63) {
    rememberNetwork(migratedNetworks, legacySsid, legacyPassword,
                    kMaximumRememberedWifiNetworks);
  }
  applyPreferredWifi(migrated, migratedNetworks);

  const bool migratedOk = validSettings(migrated) &&
      persistState(migrated, migratedNetworks);
  if (migratedOk) {
    settings_ = migrated;
    wifiNetworks_ = migratedNetworks;
  }
  log.printf("{\"event\":\"config\",\"ok\":%s,\"stage\":\"blob_migration\",\"wifi_configured\":%s,\"wifi_network_count\":%u,\"tencent_configured\":%s}\n",
             migratedOk ? "true" : "false",
             migratedOk && !migratedNetworks.empty() ? "true" : "false",
             migratedOk ? static_cast<unsigned>(migratedNetworks.size()) : 0U,
             migratedOk && !migrated.secretId.isEmpty() &&
                     !migrated.secretKey.isEmpty() ? "true" : "false");
  return migratedOk;
}

bool DeviceConfig::save(const DeviceSettings &settings, Print &log) {
  if (!open_ || !validSettings(settings)) return false;

  std::vector<WifiCredential> proposedNetworks = wifiNetworks_;
  if (settings.wifiSsid.isEmpty()) {
    // Preserve the Link v2 rescue contract: wifiSsid:null clears all saved Wi-Fi.
    proposedNetworks.clear();
  } else {
    rememberNetwork(proposedNetworks, settings.wifiSsid, settings.wifiPassword,
                    kMaximumRememberedWifiNetworks);
  }
  DeviceSettings proposedSettings = settings;
  applyPreferredWifi(proposedSettings, proposedNetworks);
  const bool ok = persistState(proposedSettings, proposedNetworks);
  if (ok) {
    settings_ = proposedSettings;
    wifiNetworks_ = proposedNetworks;
  }
  log.printf("{\"event\":\"config_saved\",\"ok\":%s,\"wifi_configured\":%s,\"wifi_network_count\":%u,\"tencent_configured\":%s}\n",
             ok ? "true" : "false",
             ok && !proposedNetworks.empty() ? "true" : "false",
             ok ? static_cast<unsigned>(proposedNetworks.size()) :
                  static_cast<unsigned>(wifiNetworks_.size()),
             ok && !proposedSettings.secretId.isEmpty() &&
                     !proposedSettings.secretKey.isEmpty() ? "true" : "false");
  return ok;
}

bool DeviceConfig::clearWifi(Print &log) {
  if (!open_) return false;
  DeviceSettings proposed = settings_;
  proposed.wifiSsid = "";
  proposed.wifiPassword = "";
  const std::vector<WifiCredential> empty;
  const bool ok = persistState(proposed, empty);
  if (ok) {
    settings_ = proposed;
    wifiNetworks_.clear();
    // Legacy keys are no longer authoritative; remove them best-effort only
    // after the valid empty blob has committed.
    preferences_.remove("wifi_ssid");
    preferences_.remove("wifi_pass");
    preferences_.remove("wifi_count");
    for (size_t index = 0; index < kMaximumRememberedWifiNetworks; ++index) {
      const String suffix(index);
      preferences_.remove(("wifi_s" + suffix).c_str());
      preferences_.remove(("wifi_p" + suffix).c_str());
    }
  }
  log.printf("{\"event\":\"wifi_credentials_cleared\",\"ok\":%s}\n",
             ok ? "true" : "false");
  return ok;
}

const WifiCredential *DeviceConfig::wifiNetwork(const String &ssid) const {
  const auto found = std::find_if(wifiNetworks_.begin(), wifiNetworks_.end(),
      [&ssid](const WifiCredential &network) { return network.ssid == ssid; });
  return found == wifiNetworks_.end() ? nullptr : &*found;
}

bool DeviceConfig::persistState(
    const DeviceSettings &settings,
    const std::vector<WifiCredential> &networks) {
  if (!open_ || !validSettings(settings) ||
      networks.size() > kMaximumRememberedWifiNetworks ||
      std::any_of(networks.begin(), networks.end(),
          [](const WifiCredential &network) {
            return network.ssid.isEmpty() || network.ssid.length() > 32 ||
                network.password.length() > 63;
          })) {
    return false;
  }
  const StoredDeviceConfig stored = encodeConfig(settings, networks);
  return preferences_.putBytes(kConfigBlobKey, &stored, sizeof(stored)) ==
      sizeof(stored);
}

bool DeviceConfig::rememberWifi(const String &ssid, const String &password,
                                Print &log) {
  if (!open_ || ssid.isEmpty() || ssid.length() > 32 || password.length() > 63) {
    return false;
  }
  std::vector<WifiCredential> updated = wifiNetworks_;
  rememberNetwork(updated, ssid, password, kMaximumRememberedWifiNetworks);
  DeviceSettings proposed = settings_;
  applyPreferredWifi(proposed, updated);
  if (!persistState(proposed, updated)) return false;
  wifiNetworks_ = updated;
  settings_ = proposed;
  log.printf("{\"event\":\"wifi_remembered\",\"count\":%u}\n",
             static_cast<unsigned>(wifiNetworks_.size()));
  return true;
}

bool DeviceConfig::forgetWifi(const String &ssid, Print &log) {
  if (!open_ || ssid.isEmpty()) return false;
  std::vector<WifiCredential> updated = wifiNetworks_;
  if (!forgetNetwork(updated, ssid)) return false;
  DeviceSettings proposed = settings_;
  applyPreferredWifi(proposed, updated);
  if (!persistState(proposed, updated)) return false;
  wifiNetworks_ = updated;
  settings_ = proposed;
  log.printf("{\"event\":\"wifi_forgotten\",\"count\":%u}\n",
             static_cast<unsigned>(wifiNetworks_.size()));
  return true;
}

bool DeviceConfig::markWifiSuccessful(const String &ssid, Print &log) {
  const WifiCredential *network = wifiNetwork(ssid);
  if (network == nullptr) return false;
  if (!wifiNetworks_.empty() && wifiNetworks_.front().ssid == ssid) return true;
  const String password = network->password;
  return rememberWifi(ssid, password, log);
}

void DeviceConfig::syncPreferredWifi() {
  applyPreferredWifi(settings_, wifiNetworks_);
}

bool DeviceConfig::clearTencent(Print &log) {
  if (!open_) return false;
  DeviceSettings proposed = settings_;
  proposed.secretId = "";
  proposed.secretKey = "";
  proposed.hotwordId = "";
  const bool ok = persistState(proposed, wifiNetworks_);
  if (ok) {
    settings_ = proposed;
    preferences_.remove("secret_id");
    preferences_.remove("secret_key");
    preferences_.remove("hotword_id");
  }
  log.printf("{\"event\":\"tencent_credentials_cleared\",\"ok\":%s}\n",
             ok ? "true" : "false");
  return ok;
}

bool DeviceConfig::setWifiEnabled(bool enabled, Print &log) {
  if (!open_) return false;
  DeviceSettings proposed = settings_;
  proposed.wifiEnabled = enabled;
  if (!persistState(proposed, wifiNetworks_)) return false;
  settings_ = proposed;
  log.printf("{\"event\":\"wifi_manual\",\"enabled\":%s}\n",
             enabled ? "true" : "false");
  return true;
}

bool DeviceConfig::setBluetoothEnabled(bool enabled, Print &log) {
  if (!open_) return false;
  DeviceSettings proposed = settings_;
  proposed.bluetoothEnabled = enabled;
  if (!persistState(proposed, wifiNetworks_)) return false;
  settings_ = proposed;
  log.printf("{\"event\":\"bluetooth_manual\",\"enabled\":%s}\n",
             enabled ? "true" : "false");
  return true;
}

bool DeviceConfig::setRaiseToWake(bool enabled, Print &log) {
  if (!open_) return false;
  DeviceSettings proposed = settings_;
  proposed.raiseToWake = enabled;
  if (!persistState(proposed, wifiNetworks_)) return false;
  settings_ = proposed;
  log.printf("{\"event\":\"raise_to_wake\",\"enabled\":%s}\n",
             enabled ? "true" : "false");
  return true;
}

bool DeviceConfig::setProvisioningPasswordMode(ProvisioningPasswordMode mode,
                                                Print &log) {
  if (!open_ || !validProvisioningPasswordMode(mode)) return false;
  DeviceSettings proposed = settings_;
  proposed.provisioningPasswordMode = mode;
  if (!persistState(proposed, wifiNetworks_)) return false;
  settings_ = proposed;
  // Never include the provisioning password in logs. The mode is safe to
  // report and makes field diagnostics explain which policy is active.
  log.printf("{\"event\":\"provisioning_password_mode\",\"mode\":%u}\n",
             static_cast<unsigned>(mode));
  return true;
}

}  // namespace pokepod
