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

assert "sessionStopRequested()" in service_h
assert "acknowledgeSessionStopRequest()" in service_h
assert "bleCallbackAllowedDuringDisable" in service
assert "if (actions.requestSessionStop) sessionStopRequest_.request();" in service
assert "bleVoice.sessionStopRequested()" in app
assert "bleVoice.acknowledgeSessionStopRequest()" in app
assert "captureRouter.owner() == AudioCaptureOwner::wirelessVoice" in app
loop_poll = app.index("bleVoice.poll(now);")
stop_adapter = app[app.index("if (bleVoice.sessionStopRequested())", loop_poll):
                   app.index("if (wirelessUiActive && !bleVoice.streaming())",
                             loop_poll)]
assert stop_adapter.index("requestCaptureStop(") < stop_adapter.index(
    "acknowledgeSessionStopRequest()"
)
assert "} else {\n      bleVoice.acknowledgeSessionStopRequest();" in stop_adapter
assert "requestCaptureStop(PendingCaptureStop::wirelessVoice" in app
assert "captureRouter.acquire(AudioCaptureOwner::wirelessVoice)" in app
assert "captureRouter.release(AudioCaptureOwner::wirelessVoice)" in app
assert ".acquire(" not in controller
assert ".release(" not in controller
assert "router_" not in controller
assert "router.owner() != AudioCaptureOwner::wirelessVoice" in controller
assert "!enablePolicy_.acceptsNewWork()" in service
assert "enablePolicy_.transitionPending()" in service_h
assert "if (!enablePolicy_.acceptsNewWork() || callbackOverflow_.active()" in service
assert "bool physicalConnectionPending() const" in service_h
assert "uint16_t physicalConnectionId() const" in service_h
for entrypoint in (
    "enablePolicy_.requestEnable(physicalConnectionPending())",
    "enablePolicy_.requestDisable(controller_.active(),\n"
    "                                   physicalConnectionPending(), nowMs)",
    "enablePolicy_.poll(controller_.active(),\n"
    "                                        physicalConnectionPending(), nowMs)",
):
    assert entrypoint in service
enable_actions = service[service.index(
    "void BleVoiceService::applyEnableActions("
):service.index("void BleVoiceService::clearDisabledRuntime(")]
assert "actions.disconnect && physicalConnectionPending()" in enable_actions
assert "!callbackOverflow_.active()" in enable_actions
assert "const uint16_t connectionId = physicalConnectionId();" in enable_actions
assert "server_->disconnect(connectionId);" in enable_actions
assert "actions.disconnect && connected_" not in enable_actions
physical_id = service_h[service_h.index("uint16_t physicalConnectionId() const"):
                        service_h.index("BLEServer *server_")]
assert physical_id.index("callbackOverflow_.physicalConnectionPending()") < \
    physical_id.index("connectionPolicy_.hasCurrent()")
assert "callbackOverflow_.epoch().connectionId" in physical_id
overflow_finish = service[service.index(
    "bool BleVoiceService::finishCallbackOverflowIfDisconnected("
):service.index("void BleVoiceService::refreshCallbackSnapshot(")]
assert overflow_finish.index("callbackOverflow_.confirm") < overflow_finish.index(
    "processDisconnect("
)
assert "callbackEvents_.resetAfterOverflow();" in overflow_finish
assert "notifyStatusEvents_.resetAfterOverflow();" in overflow_finish

assert 'UiIcon::bluetooth, "蓝牙"' in dashboard
assert "BLESecurity::regenPassKeyOnConnect(false);" in service
passkey_handler = service[
    service.index("void BleVoiceService::processPasskey"):
    service.index("bool BleVoiceService::notifyControl")
]
assert "passkey_ = passkey" not in passkey_handler
assert "ble_voice_stale_passkey_ignored" in passkey_handler
assert "ble_voice_handshake_timeout_disconnect" in service
assert 'String("等待 Mac 应用")' in dashboard
assert '"已连接 · 质量不足"' not in dashboard
assert '"请先开启蓝牙"' in dashboard
assert '"蓝牙已关闭"' in dashboard
assert "view.bluetoothEnabled" in dashboard
assert "UiAction::bluetoothToggle" in ui
assert "kDeviceBluetoothToggleLeft" in ui
assert "configStarted ? deviceConfig.settings().bluetoothEnabled" in app
assert ": false," in app
start_hold = app[app.index("bool startWirelessHold()"):
                 app.index("AudioCaptureDispatchResult drainCapturedAudio")]
assert start_hold.index("!bleVoice.userEnabled()") < start_hold.index(
    "!bleVoice.appReady()"
)
assert 'showMessage("蓝牙已关闭")' in start_hold
boot_button = app.index("if (bootButton.update(")
boot_release = app[app.index("if (bootWirelessHolding)", boot_button):
                   app.index("if (audio.playing())", boot_button)]
assert "!bleVoice.userEnabled()" in boot_release
assert 'showMessage("蓝牙已关闭")' in boot_release

print("PASS bluetooth_master_toggle_contract")
