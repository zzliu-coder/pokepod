#!/usr/bin/env python3
"""Cross-module contract for the 30s/60s/180s power lifecycle."""

from pathlib import Path


root = Path(__file__).parents[1]
firmware = root / "firmware" / "PokePodAmoled"
policy = (firmware / "PowerPolicy.h").read_text(encoding="utf-8")
runtime = (firmware / "RuntimePowerManager.cpp").read_text(encoding="utf-8")
main = (firmware / "PokePodApp.cpp").read_text(encoding="utf-8")
board = (firmware / "BoardServices.cpp").read_text(encoding="utf-8")
ble = (firmware / "BleVoiceService.cpp").read_text(encoding="utf-8")
wifi = (firmware / "WifiController.cpp").read_text(encoding="utf-8")

assert "kDefaultTimeoutMs = 30000" in policy
assert "kLightSleepTimeoutMs = 60000" in policy
assert "kDeepSleepTimeoutMs = 180000" in policy
assert "PowerMode::deepSleepPending" in policy
assert "PowerMode::safeShutdownPending" in policy
assert "powerForegroundBusy(input)" in policy
assert "input.criticalBattery && !input.vbusPresent" in policy
assert "!input.automaticWakeEnabled" in policy

assert "esp_sleep_enable_ext1_wakeup_io" in runtime
assert "wakeMask = 1ULL << kBootButtonPin" in runtime
assert "wakeMask |= 1ULL << kTouchInterruptPin" in runtime
assert "ESP_EXT1_WAKEUP_ANY_LOW" in runtime
assert "esp_deep_sleep_start" in runtime

assert "input.usbHostConnected = usb.hostConnected()" in main
assert "input.vbusPresent = board.status().vbusPresent" in main
assert "wirelessSync.openWindow()" in main
assert "pauseIdleRadios" in main
assert "enterDeepSleep" in main
assert "performSafeShutdown" in main
assert "SD_MMC.end()" in main
assert "lowBatteryShutdown.critical()" in main

assert "pauseForIdleSleep" in ble
assert "resumeAfterIdleSleep" in ble
assert "idlePaused_" in ble
assert "prepareForDeepSleep" in ble
assert "prepareForSleep" in wifi
assert "prepareForDeepSleep" in board

print("PASS test_three_stage_power_contract")
