#!/usr/bin/env python3
"""Integration contract for power, per-connection time and physical USB UI."""

from pathlib import Path


root = Path(__file__).parents[1]
firmware = root / "firmware" / "PokePodAmoled"
main = (firmware / "PokePodApp.cpp").read_text(encoding="utf-8")
wifi = (firmware / "WifiController.cpp").read_text(encoding="utf-8")
power = (firmware / "RuntimePowerManager.cpp").read_text(encoding="utf-8")
policy = (firmware / "PowerPolicy.h").read_text(encoding="utf-8")
dashboard = (firmware / "Dashboard.cpp").read_text(encoding="utf-8")
ble = (firmware / "BleVoiceService.cpp").read_text(encoding="utf-8")

assert "sntp_set_time_sync_notification_cb" in wifi
assert "timeSyncState_.noteConnected(connected_)" in wifi
assert "timeSyncState_.noteSynchronized()" in wifi
assert "wifi.networkTimeSyncRevision()" in main
assert "board.setUtcEpoch(synchronizedEpoch)" in main
assert "network_time_applied" in main

assert "usbPhysicalConnected(usb.hostConnected(), status.pmu" in main
assert "view.usbConnected = usbCableConnected();" in main
assert "lastVbusPresent" in main

assert "automaticWakeEnabled() && touchInterrupt" in main
assert "verifiedInputs.automaticWakeEnabled" in power
assert "kLightSleepSliceUs = 500000" in power
assert "!input.wifiRadioOn" in policy
assert "input.wifiRadioOn || input.linkBusy" not in policy

assert '"自动亮屏"' in dashboard
assert '"触摸或抬起"' in dashboard
assert "kDimScreenBrightness" in main
assert "BleConnectionPowerMode::voice" in ble
assert "BleConnectionPowerMode::idle" in ble

print("PASS test_runtime_power_time_usb_contract")
