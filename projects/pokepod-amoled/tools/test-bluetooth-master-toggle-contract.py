#!/usr/bin/env python3
"""Pin the persistent Bluetooth master switch production wiring."""

from pathlib import Path


project = Path(__file__).parents[1]
firmware = project / "firmware/PokePodAmoled"
app = (firmware / "PokePodApp.cpp").read_text(encoding="utf-8")
service = (firmware / "BleVoiceService.cpp").read_text(encoding="utf-8")
service_h = (firmware / "BleVoiceService.h").read_text(encoding="utf-8")
controller = (firmware / "VoiceSessionController.h").read_text(encoding="utf-8")
config = (firmware / "DeviceConfig.cpp").read_text(encoding="utf-8")
blob = (firmware / "DeviceConfigBlob.h").read_text(encoding="utf-8")
dashboard = (firmware / "Dashboard.cpp").read_text(encoding="utf-8")
ui = (firmware / "UiPolicy.h").read_text(encoding="utf-8")

assert "stored.reserved[0] = settings.bluetoothEnabled" in config
assert "storedDeviceConfigBluetoothEnabled(stored)" in config
assert "kStoredBluetoothLegacyEnabled = 0" in blob
assert "kStoredBluetoothEnabled = 1" in blob
assert "kStoredBluetoothDisabled = 2" in blob

toggle = app[app.index("action == UiAction::bluetoothToggle"):
             app.index("action == UiAction::openBluetoothPairing")]
assert toggle.index("setBluetoothEnabled(enabled") < toggle.index(
    "bleVoice.requestEnable()"
)
assert toggle.index("setBluetoothEnabled(enabled") < toggle.index(
    "bleVoice.requestDisable(now)"
)
assert "蓝牙设置保存失败" in toggle

assert "takeSessionStopRequested()" in service_h
assert "bleCallbackAllowedDuringDisable" in service
assert "if (actions.requestSessionStop) sessionStopRequested_ = true;" in service
assert "sessionStopRequested_ = false;" not in service
assert "bleVoice.takeSessionStopRequested()" in app
assert "captureRouter.owner() == AudioCaptureOwner::wirelessVoice" in app
stop_adapter = app[app.index(
    "if (captureRouter.owner() == AudioCaptureOwner::wirelessVoice &&"
):app.index(
    "if (wirelessUiActive && !bleVoice.streaming())"
)]
assert stop_adapter.index("captureRouter.owner()") < stop_adapter.index(
    "takeSessionStopRequested()"
)
assert "requestCaptureStop(PendingCaptureStop::wirelessVoice" in app
assert "captureRouter.acquire(AudioCaptureOwner::wirelessVoice)" in app
assert "captureRouter.release(AudioCaptureOwner::wirelessVoice)" in app
assert ".acquire(" not in controller
assert ".release(" not in controller
assert "router_" not in controller
assert "router.owner() != AudioCaptureOwner::wirelessVoice" in controller
assert "!enablePolicy_.acceptsNewWork()" in service
assert "enablePolicy_.transitionPending()" in service_h
assert "if (!enablePolicy_.acceptsNewWork() || idlePaused_" in service

assert 'UiIcon::bluetooth, "蓝牙"' in dashboard
assert '"请先开启蓝牙"' in dashboard
assert "view.bluetoothEnabled" in dashboard
assert "UiAction::bluetoothToggle" in ui
assert "kDeviceBluetoothToggleLeft" in ui
assert "configStarted ? deviceConfig.settings().bluetoothEnabled" in app
assert ": false," in app

print("PASS bluetooth_master_toggle_contract")
