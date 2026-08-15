#!/usr/bin/env python3
"""Source contract for USB OTA and its rescue boundary."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "PokePodAmoled"
CDC = ROOT / "cdc-status.py"
FIXTURE = ROOT / "fixture" / "pokepod-fixture.py"


def main() -> int:
    session = (FIRMWARE / "FirmwareUpdateSession.cpp").read_text()
    policy = (FIRMWARE / "FirmwareUpdatePolicy.h").read_text()
    service = (FIRMWARE / "PokePodLinkService.cpp").read_text()
    dispatcher = (FIRMWARE / "LinkCommandDispatcher.cpp").read_text()
    transport = (FIRMWARE / "LinkTransportSession.cpp").read_text()
    cdc = CDC.read_text()
    fixture = FIXTURE.read_text()
    assert "esp_ota_get_next_update_partition" in session
    assert "esp_ota_begin" in session and "esp_ota_write" in session
    assert "esp_ota_end" in session and "esp_ota_set_boot_partition" in session
    assert "esp_ota_abort" in session and "SHA-256 mismatch" in session
    assert "kMaximumImageBytes = 0x300000U" in policy
    assert "firmware-update" in dispatcher and "LinkTransport::usb" in dispatcher
    assert "binary_ack" in service and "finishFirmwareUpdate" in service
    assert "LinkOperationResource::firmwareUpdate" in service
    assert "firmwareUpdate_.abort()" in transport and "!firmwareUpdate_.active()" in transport
    assert "--firmware" in cdc and "hashlib.sha256" in cdc
    assert "--identity-authority" in fixture and "--firmware" in fixture
    assert "flash.sh" in fixture and "rescue" in fixture
    print("PASS test-firmware-update")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
