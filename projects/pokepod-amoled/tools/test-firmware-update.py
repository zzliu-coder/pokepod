#!/usr/bin/env python3
"""Source contract for USB OTA and its rescue boundary."""

import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "PokePodAmoled"
CDC = ROOT / "cdc-status.py"
FIXTURE = ROOT / "fixture" / "pokepod-fixture.py"
IDENTITY_FORMAT = "<8sHH24s16s41sB3s41s65s"
IDENTITY_BYTES = struct.calcsize(IDENTITY_FORMAT)


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def closed_artifact(output: Path) -> tuple[Path, Path, dict[str, object], dict[str, str]]:
    source_revision = "d" * 40
    source_tree = "f" * 40
    firmware_version = "2.0.0"
    elf = output / "PokePodAmoled.ino.elf"
    elf.write_bytes(b"exact-candidate-elf")
    app_elf_sha256 = hashlib.sha256(elf.read_bytes()).hexdigest()
    identity = struct.pack(
        IDENTITY_FORMAT,
        b"PKPDIMG2",
        2,
        IDENTITY_BYTES,
        b"PokePodAmoled\0".ljust(24, b"\0"),
        (firmware_version + "\0").encode().ljust(16, b"\0"),
        (source_revision + "\0").encode(),
        0,
        b"\0" * 3,
        (source_tree + "\0").encode(),
        b"unknown\0".ljust(65, b"\0"),
    )
    firmware = output / "PokePodAmoled.ino.bin"
    firmware.write_bytes(b"\x00" * 96 + identity + b"\xa5" * 1024)
    policy_module = load_module(
        "pokepod_flash_size_policy_for_ota_test",
        ROOT / "tools" / "flash-size-policy.py",
    )
    policy = policy_module.evaluate(firmware.stat().st_size, 0x300000)
    expected = {
        "sourceRevision": source_revision,
        "firmwareVersion": firmware_version,
        "appElfSha256": app_elf_sha256,
    }
    manifest: dict[str, object] = {
        "schemaVersion": 1,
        "kind": "hardmac.artifact",
        "lane": "fast",
        "sourceRevision": source_revision,
        "sourceDirty": False,
        "sourceTree": source_tree,
        "firmwareVersion": firmware_version,
        "imageIdentity": {
            "magic": "PKPDIMG2",
            "schema": 2,
            "product": "PokePodAmoled",
            "sourceRevision": source_revision,
            "sourceTree": source_tree,
            "firmwareVersion": firmware_version,
            "sourceDirty": False,
            "appElfSha256": app_elf_sha256,
        },
        "binary": {
            "file": firmware.name,
            "sizeBytes": firmware.stat().st_size,
            "sha256": hashlib.sha256(firmware.read_bytes()).hexdigest(),
            "flashOffset": "0x10000",
            "slotSizeBytes": 0x300000,
            "remainingBytes": policy["remainingBytes"],
            "usagePercent": policy["percent"],
            "resourceTier": policy["tier"],
        },
        "resourcePolicy": policy,
        "resourceReview": {
            "required": False,
            "approved": False,
            "evidenceFile": "../../resource-review.json",
        },
        "toolchain": {
            "fqbn": "esp32:esp32:esp32s3",
            "coreProfile": "production",
            "esp32ArduinoCore": "3.3.8",
        },
    }
    return firmware, elf, manifest, expected


def main() -> int:
    session = (FIRMWARE / "FirmwareUpdateSession.cpp").read_text()
    session_header = (FIRMWARE / "FirmwareUpdateSession.h").read_text()
    policy = (FIRMWARE / "FirmwareUpdatePolicy.h").read_text()
    service = (FIRMWARE / "PokePodLinkService.cpp").read_text()
    dispatcher = (FIRMWARE / "LinkCommandDispatcher.cpp").read_text()
    transport = (FIRMWARE / "LinkTransportSession.cpp").read_text()
    cdc = CDC.read_text()
    fixture = FIXTURE.read_text()
    assert "esp_ota_get_next_update_partition" in session
    assert "esp_ota_begin" in session and "esp_ota_write" in session
    assert "esp_ota_begin(target_, OTA_WITH_SEQUENTIAL_WRITES, &handle_)" in session
    assert "esp_ota_begin(target_, expectedBytes" not in session
    assert "esp_ota_end" in session and "esp_ota_set_boot_partition" in session
    assert "esp_ota_get_partition_description" in session
    assert "candidate app ELF SHA-256 mismatch" in session
    assert session.index("if (!validateCandidateAppElfSha256())") < session.index(
        "if (esp_ota_end(handle_)", session.index("bool FirmwareUpdateSession::finish")
    )
    assert "esp_ota_abort" in session and "SHA-256 mismatch" in session
    assert "bool begin(uint32_t expectedBytes, const char *expectedSha256);" not in session_header
    assert "return begin(expectedBytes, expectedSha256, nullptr" not in session
    assert "kMaximumImageBytes = 0x300000U" in policy
    assert "firmware-update" in dispatcher and "LinkTransport::usb" in dispatcher
    assert "validSourceRevision(expectedSourceRevision)" in dispatcher
    assert "validFirmwareVersion(expectedFirmwareVersion)" in dispatcher
    assert "validSha256(expectedAppElfSha256)" in dispatcher
    assert "binary_ack\\\",\\\"received\\\":0" in dispatcher
    assert "binary_ack" in service and "finishFirmwareUpdate" in service
    assert "LinkOperationResource::firmwareUpdate" in service
    assert "firmwareUpdate_.abort()" in transport and "!firmwareUpdate_.active()" in transport
    assert "--firmware" in cdc and "hashlib.sha256" in cdc
    assert "firmware_update_fields" in cdc
    assert "validate_firmware_query_fields" in cdc
    assert "load_adjacent_artifact_identity" in cdc
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
    try:
        cdc_module.firmware_update_fields(digest)
    except ValueError as error:
        assert "artifact.json" in str(error)
    else:
        raise AssertionError("unbound OTA fields were accepted")
    try:
        cdc_module.query("/definitely-not-a-device", "firmware-update", 1.0,
                         fields={"sha256": digest})
    except ValueError as error:
        assert "identity" in str(error)
    else:
        raise AssertionError("low-level unbound OTA query was accepted")
    bound_query = {
        "sha256": digest,
        "sourceRevision": "b" * 40,
        "firmwareVersion": "2.0.0",
        "appElfSha256": "c" * 64,
    }
    try:
        cdc_module.query("/definitely-not-a-device", "firmware-update", 1.0,
                         fields=bound_query)
    except OSError:
        pass
    except ValueError as error:
        raise AssertionError(f"bound OTA query rejected before transport: {error}")
    else:
        raise AssertionError("bound OTA query unexpectedly reached a fake port")
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
        firmware, elf, manifest, expected = closed_artifact(output)
        manifest_path = output / "artifact.json"
        try:
            fixture_module.load_firmware_artifact_identity(firmware)
        except RuntimeError as error:
            assert "artifact.json" in str(error)
        else:
            raise AssertionError("missing artifact manifest was accepted")
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        assert cdc_module.load_adjacent_artifact_identity(firmware) == expected
        assert fixture_module.load_firmware_artifact_identity(firmware) == expected
        assert cdc_module.resolve_firmware_artifact_identity(firmware) == expected
        assert cdc_module.resolve_firmware_artifact_identity(
            firmware,
            expected["sourceRevision"].upper(),
            expected["firmwareVersion"],
            expected["appElfSha256"].upper(),
        ) == expected
        try:
            cdc_module.resolve_firmware_artifact_identity(
                firmware, "a" * 40, expected["firmwareVersion"],
                expected["appElfSha256"],
            )
        except ValueError as error:
            assert "does not match" in str(error)
        else:
            raise AssertionError("explicit identity bypassed the artifact")

        original_elf = elf.read_bytes()
        elf.unlink()
        try:
            cdc_module.load_adjacent_artifact_identity(firmware)
        except ValueError as error:
            assert "artifact_elf_missing" in str(error)
        else:
            raise AssertionError("missing ELF was accepted by cdc")
        elf.write_bytes(original_elf)
        elf.write_bytes(b"replaced-elf")
        try:
            cdc_module.load_adjacent_artifact_identity(firmware)
        except ValueError as error:
            assert "artifact_image_identity_elf_sha256" in str(error)
        else:
            raise AssertionError("replaced ELF was accepted by cdc")
        elf.write_bytes(original_elf)

        for mutate, expected_error in (
            (lambda value: value["toolchain"].update(
                {"esp32ArduinoCore": "3.3.7"}), "production ESP32 Arduino core 3.3.8"),
            (lambda value: value.update({"sourceTree": "1" * 40}),
             "artifact_identity_source"),
            (lambda value: value["resourcePolicy"].update(
                {"programBytes": 1}), "artifact_resource_policy_mismatch"),
            (lambda value: value["resourceReview"].update(
                {"required": True}), "artifact_resource_review_required"),
        ):
            invalid = json.loads(json.dumps(manifest))
            mutate(invalid)
            manifest_path.write_text(json.dumps(invalid), encoding="utf-8")
            try:
                cdc_module.load_adjacent_artifact_identity(firmware)
            except ValueError as error:
                assert expected_error in str(error)
            else:
                raise AssertionError(
                    f"invalid closed artifact was accepted: {expected_error}"
                )
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
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
            try:
                cdc_module.load_adjacent_artifact_identity(firmware)
            except ValueError as error:
                assert field in str(error)
            else:
                raise AssertionError(f"missing {field} was accepted by cdc")
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
        try:
            cdc_module.load_adjacent_artifact_identity(firmware)
        except ValueError as error:
            assert "artifact_image_identity_elf" in str(error)
        else:
            raise AssertionError("wrong app ELF digest was accepted by cdc")
    print("PASS test-firmware-update")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
