#!/usr/bin/env python3
"""Source contract for USB OTA and its rescue boundary."""

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "PokePodAmoled"
CDC = ROOT / "cdc-status.py"
FIXTURE = ROOT / "fixture" / "pokepod-fixture.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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
    assert "binary_ack\\\",\\\"received\\\":0" in dispatcher
    assert "binary_ack" in service and "finishFirmwareUpdate" in service
    assert "LinkOperationResource::firmwareUpdate" in service
    assert "firmwareUpdate_.abort()" in transport and "!firmwareUpdate_.active()" in transport
    assert "--firmware" in cdc and "hashlib.sha256" in cdc
    assert "firmware_update_fields" in cdc
    assert "--source-revision" in cdc and "--firmware-version" in cdc
    assert "--app-elf-sha256" in cdc
    assert "load_firmware_artifact_identity" in fixture
    assert "expectedIdentity" in fixture
    assert "max(60.0, timeout)" in cdc
    assert "--identity-authority" in fixture and "--firmware" in fixture
    assert "flash.sh" in fixture and "rescue" in fixture

    sys.path.insert(0, str(FIXTURE.parent))
    cdc_module = load_module("pokepod_cdc_status_test", CDC)
    fixture_module = load_module("pokepod_fixture_test", FIXTURE)
    digest = "a" * 64
    assert cdc_module.firmware_update_fields(digest) == {"sha256": digest}
    bound = cdc_module.firmware_update_fields(
        digest, "B" * 40, "2.0.0", "C" * 64
    )
    assert bound["sourceRevision"] == "b" * 40
    assert bound["firmwareVersion"] == "2.0.0"
    assert bound["appElfSha256"] == "c" * 64
    for values, message in (
        (("a" * 40, None, None), "requires"),
        (("a" * 39, "2.0.0", "b" * 64), "40-character"),
        (("a" * 40, "2.0.0", "b" * 63), "64-character"),
    ):
        try:
            cdc_module.firmware_update_fields(digest, *values)
        except ValueError as error:
            assert message in str(error)
        else:
            raise AssertionError("invalid OTA identity was accepted")

    with tempfile.TemporaryDirectory(prefix="pokepod-ota-contract-") as raw:
        output = Path(raw)
        firmware = output / "PokePodAmoled.ino.bin"
        payload = b"firmware-image"
        firmware.write_bytes(payload)
        expected = {
            "sourceRevision": "d" * 40,
            "firmwareVersion": "2.0.0",
            "appElfSha256": "e" * 64,
        }
        manifest = {
            "schemaVersion": 1,
            "kind": "hardmac.artifact",
            "sourceRevision": expected["sourceRevision"],
            "sourceDirty": False,
            "firmwareVersion": expected["firmwareVersion"],
            "imageIdentity": {
                "magic": "PKPDIMG2",
                "schema": 2,
                "product": "PokePodAmoled",
                "sourceRevision": expected["sourceRevision"],
                "firmwareVersion": expected["firmwareVersion"],
                "sourceDirty": False,
                "appElfSha256": expected["appElfSha256"],
            },
            "binary": {
                "file": firmware.name,
                "sizeBytes": len(payload),
                "sha256": hashlib.sha256(payload).hexdigest(),
            },
        }
        manifest_path = output / "artifact.json"
        try:
            fixture_module.load_firmware_artifact_identity(firmware)
        except RuntimeError as error:
            assert "artifact.json" in str(error)
        else:
            raise AssertionError("missing artifact manifest was accepted")
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        assert fixture_module.load_firmware_artifact_identity(firmware) == expected
        for field in ("sourceRevision", "firmwareVersion", "appElfSha256"):
            invalid = json.loads(manifest_path.read_text(encoding="utf-8"))
            invalid["imageIdentity"].pop(field)
            manifest_path.write_text(json.dumps(invalid), encoding="utf-8")
            try:
                fixture_module.load_firmware_artifact_identity(firmware)
            except RuntimeError as error:
                assert field in str(error)
            else:
                raise AssertionError(f"missing {field} was accepted")
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        invalid = json.loads(manifest_path.read_text(encoding="utf-8"))
        invalid["imageIdentity"]["appElfSha256"] = "f" * 63
        manifest_path.write_text(json.dumps(invalid), encoding="utf-8")
        try:
            fixture_module.load_firmware_artifact_identity(firmware)
        except RuntimeError as error:
            assert "appElfSha256" in str(error)
        else:
            raise AssertionError("wrong app ELF digest was accepted")
    print("PASS test-firmware-update")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
