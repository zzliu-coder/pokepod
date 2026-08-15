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

assert "usb.tinyUsbMounted()," in main
assert "usb.cdcSessionActive()," in main
assert "board.status().vbusPresent," in main
assert "input = powerInputsWithFacts(input, facts)" in main
assert "wirelessSync->openWindow()" in main
assert "pauseIdleRadios" in main
assert "enterDeepSleep" in main
deep_sleep = main[main.index("void enterDeepSleep("):
                  main.index("void requestSafeShutdown(")]
assert deep_sleep.count("!bleVoice.quiescedForSleep() || wifi.radioOn()") == 2
assert deep_sleep.rindex("!bleVoice.quiescedForSleep()") < deep_sleep.index(
    "bleVoice.prepareForDeepSleep()"
)
assert deep_sleep.rindex("!bleVoice.quiescedForSleep()") > deep_sleep.index(
    "wifi.prepareForSleep()"
)
deep_sleep_gate_start = main.rindex("if (!safeShutdownQuiesce.pending() &&")
deep_sleep_admission = main[deep_sleep_gate_start:
                            main.index("if (dashboard.advanceVerticalScroll",
                                       deep_sleep_gate_start)]
assert "bleVoice.quiescedForSleep()" in deep_sleep_admission
assert "!bleVoice.connected()" not in deep_sleep_admission
assert "requestSafeShutdown" in main
assert "advanceSafeShutdown" in main
assert main.index("tencentWorker.quiesce") < main.index("board.endSdMount()")
assert "board.endSdMount()" in main
assert "lowBatteryShutdown.critical()" in main

assert "pauseForIdleSleep" in ble
assert "return quiescedForSleep();" in ble
assert "resumeAfterIdleSleep" in ble
assert "idlePaused_" in ble
assert "prepareForDeepSleep" in ble
assert "prepareForSleep" in wifi
assert "prepareForDeepSleep" in board

print("PASS test_three_stage_power_contract")
