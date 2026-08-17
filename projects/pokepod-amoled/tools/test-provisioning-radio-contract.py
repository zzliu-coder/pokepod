#!/usr/bin/env python3
"""Production contract for releasing BLE internal RAM before SoftAP."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "PokePodAmoled"
coordinator = (FW / "ProvisioningCoordinator.cpp").read_text()
coordinator_header = (FW / "ProvisioningCoordinator.h").read_text()
policy = (FW / "ProvisioningStartupPolicy.h").read_text()
ble = (FW / "BleVoiceService.cpp").read_text()
ble_header = (FW / "BleVoiceService.h").read_text()
app = (FW / "PokePodApp.cpp").read_text()

assert "bindBleVoice" in coordinator_header
assert "pauseForIdleSleep()" in coordinator
assert "suspendForProvisioning()" in coordinator
assert "startup_.update(nowMs, radiosQuiesced)" in coordinator
assert "phase_ == ProvisioningStartupPhase::quiescing && radiosQuiesced" in policy

suspend = ble[ble.index("bool BleVoiceService::suspendForProvisioning()"):
              ble.index("void BleVoiceService::prepareForDeepSleep()")]
assert "!idlePaused_ || !quiescedForSleep()" in suspend
assert "BLEDevice::deinit(false)" in suspend
for pointer in ("server_", "info_", "command_", "event_", "audio_"):
    assert f"{pointer} = nullptr;" in suspend
assert "provisioningSuspended_ = true" in suspend
assert "if (provisioningSuspended_) return;" in ble
assert "if (provisioningSuspended_) return false;" in ble_header

restart = coordinator[coordinator.index("bool ProvisioningCoordinator::takeRestartRequired()"):
                      coordinator.index("bool ProvisioningCoordinator::active() const")]
assert "startup_.pending() || startup_.active()" in restart
assert "deviceReboot.requestLocal" in app
assert "provisioningCoordinator.bindBleVoice(bleVoice)" in app
provisioning_entry = app[app.index("action == UiAction::openProvisioning"):
                         app.index("action == UiAction::openShutdownConfirm")]
assert "storageBootPhase != StorageBootPhase::ready" in provisioning_entry

print("PASS test-provisioning-radio-contract")
