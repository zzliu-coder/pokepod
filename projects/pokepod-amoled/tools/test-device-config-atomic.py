#!/usr/bin/env python3
"""Static contracts for atomic configuration persistence and Wi-Fi compatibility."""

from pathlib import Path


firmware = Path(__file__).parents[1] / "firmware" / "PokePodAmoled"
config = (firmware / "DeviceConfig.cpp").read_text(encoding="utf-8")
blob = (firmware / "DeviceConfigBlob.h").read_text(encoding="utf-8")
wifi = (firmware / "WifiController.cpp").read_text(encoding="utf-8")
portal = (firmware / "ProvisioningPortal.cpp").read_text(encoding="utf-8")
link = (firmware / "PokePodLinkService.cpp").read_text(encoding="utf-8")

# Every live mutation commits one CRC-protected NVS blob. Legacy keys are only
# read for migration or removed best-effort after a committed clear.
assert 'constexpr char kConfigBlobKey[] = "config_v1";' in config
assert "offsetof(StoredDeviceConfig, crc32)" in blob
assert "validateDeviceConfigBlob(stored)" in config
assert config.count("putBytes(kConfigBlobKey") == 1
assert "putString(" not in config
assert "putBool(" not in config
assert "putUChar(" not in config

save = config[config.index("bool DeviceConfig::save("):
              config.index("bool DeviceConfig::clearWifi(")]
assert "proposedNetworks.clear();" in save
assert "persistState(proposedSettings, proposedNetworks)" in save
assert save.index("persistState(proposedSettings, proposedNetworks)") < save.index(
    "settings_ = proposedSettings"
)

clear_wifi = config[config.index("bool DeviceConfig::clearWifi("):
                    config.index("const WifiCredential *")]
assert clear_wifi.index("persistState(proposed, empty)") < clear_wifi.index(
    "wifiNetworks_.clear()"
)

# Link null retains its v2 rescue meaning: it becomes an empty String and the
# atomic save path clears all remembered networks.
assert "else if (cJSON_IsNull(item)) *field.target = \"\";" in link
assert "settings.wifiSsid.isEmpty()" in save

# Failed MRU writes remain retryable, and portal distinguishes missing entries
# from NVS storage failures.
assert "if (config_->markWifiSuccessful(WiFi.SSID(), *log_))" in wifi
assert "kWifiMruPersistRetryMs = 5000" in wifi
assert "nowMs - lastMruPersistAttemptMs_" in wifi
forget = portal[portal.index("void ProvisioningPortal::forgetRequest()"):
                portal.index("void ProvisioningPortal::sendSaveJson")]
assert "config_->wifiNetwork(ssid) == nullptr" in forget
assert 'server_.send(404' in forget
assert 'server_.send(500' in forget

print("PASS test_device_config_atomic")
