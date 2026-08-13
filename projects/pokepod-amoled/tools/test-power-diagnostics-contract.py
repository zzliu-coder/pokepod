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
power_diagnostics = (firmware / "PowerDiagnostics.cpp").read_text(encoding="utf-8")
power_codec = (firmware / "PowerDiagnosticsCodec.h").read_text(encoding="utf-8")
link = (firmware / "PokePodLinkService.cpp").read_text(encoding="utf-8")
link_dispatcher = (firmware / "LinkCommandDispatcher.cpp").read_text(encoding="utf-8")
link_surface = link + link_dispatcher
link_diagnostics = (firmware / "LinkDiagnostics.cpp").read_text(encoding="utf-8")
dashboard = (firmware / "Dashboard.cpp").read_text(encoding="utf-8")
cdc_status = (root / "cdc-status.py").read_text(encoding="utf-8")

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
assert "const PowerFacts facts" in main
assert "usb.tinyUsbMounted()" in main
assert "usb.cdcSessionActive()" in main
assert "board.status().vbusPresent" in main
assert "board.status().charging" in main
assert "linkLeaseActive" in main
assert "powerInputsWithFacts(input, facts)" in main
assert "esp_light_sleep_start" in power
assert "esp_deep_sleep_start" in power
assert "kLightSleepTimeoutMs = 60000" in policy
assert "kDeepSleepTimeoutMs = 180000" in policy
assert "prepareForDeepSleep" in board
assert "LowBatteryShutdownPolicy" in policy
assert "requestSafeShutdown" in main
assert "advanceSafeShutdown" in main
assert main.index("tencentWorker.quiesce") < main.index("SD_MMC.end()")
assert "setCpuFrequencyMhz" in power
assert "CONFIG_PM_ENABLE" in power
assert "CONFIG_BT_CTRL_MODEM_SLEEP" in power
assert "esp_pm_configure" not in power
assert "esp_wifi_set_ps" in wifi
assert "WIFI_PS_MIN_MODEM" in wifi
assert "WIFI_PS_NONE" in portal
assert "provisioning-diagnostics" in link_surface
assert "get-provisioning-diagnostics" in link_surface
assert "clear-provisioning-diagnostics" in link_surface
assert "power-diagnostics" in link_surface
assert "get-power-diagnostics" in link_surface
assert "clear-power-diagnostics" in link_surface
assert "diagnostics_.powerJson()" in link_surface
assert "provisioning-start" in link_dispatcher
assert "provisioning-stop" in link_surface
assert "resetReason" in link_diagnostics
assert "provisioningStartupPhase" in link_diagnostics
assert "internalHeapFree" in link_diagnostics
assert "internalHeapLargest" in link_diagnostics
assert "psramFree" in link_diagnostics
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
export_body = link_diagnostics[
    link_diagnostics.index("String LinkDiagnostics::provisioningJson") :
    link_diagnostics.index("String LinkDiagnostics::powerJson")
]
for secret in ("password", "secretId", "secretKey", "hotword"):
    assert secret not in export_body

assert 'preferences_.begin("pokepod_diag", false)' in power_diagnostics
assert 'kPowerLogKey[] = "power_log_v1"' in power_diagnostics
assert "shouldPersistBlockedObservation" in power_diagnostics
assert "shouldPersistLightWake" in power_diagnostics
assert "recordAutomaticScreenWake" in main
assert "AutomaticScreenWakeSource::touchInterrupt" in main
assert "AutomaticScreenWakeSource::motionInterrupt" in main
assert "AutomaticScreenWakeSource::raiseToWakePolicy" in main
assert "recordDeepSleepIntent" in main
assert main.index("recordDeepSleepIntent") < main.index("SD_MMC.end()")
assert "kPowerErrorBootWakeLineHeld" in power
assert "digitalRead(kBootButtonPin) == LOW" in power
assert "esp_sleep_get_ext1_wakeup_status" in power
assert "esp_sleep_get_wakeup_causes" in power
assert "sizeof(StoredPowerLog) < 768" in power_codec
assert "kPowerLogCapacity = 10" in power_codec
assert 'operation == "get-power-diagnostics"' in cdc_status
assert 'record["blockers"]' in cdc_status
for secret in ("password", "secretId", "secretKey", "hotword"):
    assert secret not in power_diagnostics
power_export = link_diagnostics[
    link_diagnostics.index("String LinkDiagnostics::powerJson") :
    link_diagnostics.index("bool LinkDiagnostics::clearProvisioning")
]
for secret in ("password", "secretId", "secretKey", "hotword"):
    assert secret not in power_export

assert "!input.bleConnected" in policy
assert "!input.wifiRadioOn" in policy
assert "!input.usbHostConnected" in policy
assert "!input.vbusPresent" in policy
assert "!input.automaticWakeEnabled" in policy
print("PASS test_power_diagnostics_contract")
