#!/usr/bin/env python3
"""Static safety contract for power gating and provisioning diagnostics."""

from pathlib import Path


root = Path(__file__).parents[1]
firmware = root / "firmware" / "PokePodAmoled"
audio = (firmware / "AudioPipeline.cpp").read_text(encoding="utf-8")
board = (firmware / "BoardServices.cpp").read_text(encoding="utf-8")
main = (firmware / "PokePodApp.cpp").read_text(encoding="utf-8")
power = (firmware / "RuntimePowerManager.cpp").read_text(encoding="utf-8")
policy = (firmware / "PowerPolicy.h").read_text(encoding="utf-8")
wifi = (firmware / "WifiController.cpp").read_text(encoding="utf-8")
portal = (firmware / "ProvisioningPortal.cpp").read_text(encoding="utf-8")
diagnostics = (firmware / "ProvisioningDiagnostics.cpp").read_text(encoding="utf-8")
link = (firmware / "PokePodLinkService.cpp").read_text(encoding="utf-8")
dashboard = (firmware / "Dashboard.cpp").read_text(encoding="utf-8")

assert "i2s_.end()" in audio
assert "es8311_delete" in audio
assert "audio_power" in audio
assert "display_->displayOff()" in board
assert "display_->displayOn()" in board
assert "configWakeOnMotion" in board
assert "ACC_ODR_LOWPOWER_21Hz" in board
assert "AutoScreenOffPolicy" in main
assert "runtimePower.enterLightSleep" in main
assert "automaticWakeEnabled()" in main
assert "kDimScreenBrightness" in main
assert "input.vbusPresent" in main
assert "input.usbHostConnected" in main
assert "input.linkBusy" in main
assert "esp_light_sleep_start" in power
assert "kLightSleepSliceUs = 500000" in power
assert "setCpuFrequencyMhz" in power
assert "CONFIG_PM_ENABLE" in power
assert "CONFIG_BT_CTRL_MODEM_SLEEP" in power
assert "esp_pm_configure" not in power
assert "esp_wifi_set_ps" in wifi
assert "WIFI_PS_MIN_MODEM" in wifi
assert "WIFI_PS_NONE" in portal
assert "provisioning-diagnostics" in link
assert "get-provisioning-diagnostics" in link
assert "clear-provisioning-diagnostics" in link
assert "provisioning-start" in link
assert "provisioning-stop" in link
assert "resetReason" in link
assert "provisioningStartupPhase" in link
assert "internalHeapFree" in link
assert "internalHeapLargest" in link
assert "psramFree" in link
assert "internal_heap_free" in main
assert "internal_heap_largest" in main
assert "psram_free" in main
assert "drawProvisioningLog" in dashboard
assert "诊断记录" in dashboard

# The persistent record and USB export may contain SSID and reason metadata.
# Credentials and Tencent secrets must not enter either surface.
record_body = diagnostics[
    diagnostics.index("bool ProvisioningDiagnostics::record") :
    diagnostics.index("bool ProvisioningDiagnostics::clear")
]
for secret in ("password", "secretId", "secretKey", "hotword"):
    assert secret not in record_body
export_body = link[
    link.index("String PokePodLinkService::provisioningDiagnosticsJson") :
    link.index("bool PokePodLinkService::executeCommand")
]
for secret in ("password", "secretId", "secretKey", "hotword"):
    assert secret not in export_body

assert "!input.bleConnected" in policy
assert "!input.wifiRadioOn" in policy
assert "!input.usbHostConnected" in policy
assert "!input.vbusPresent" in policy
print("PASS test_power_diagnostics_contract")
