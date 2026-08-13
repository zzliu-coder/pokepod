#!/usr/bin/env python3
"""Exercise the fail-closed PokePod flash identity gate without hardware."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = ROOT / "tools/validate-flash-identity.py"
FLASH = (ROOT / "flash.sh").read_text(encoding="utf-8")


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value) + "\n", encoding="utf-8")


def run(*arguments: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(VALIDATOR), *map(str, arguments)],
        text=True,
        capture_output=True,
        check=False,
    )


def expect_failure(
    expected: str, *arguments: object
) -> subprocess.CompletedProcess[str]:
    result = run(*arguments)
    assert result.returncode != 0, result.stdout
    assert expected in result.stderr, result.stderr
    return result


missing_authority = subprocess.run(
    ["sh", str(ROOT / "flash.sh"), "--release"],
    text=True,
    capture_output=True,
    check=False,
)
assert missing_authority.returncode == 64
assert "FAIL identity_authority_required" in missing_authority.stderr


with tempfile.TemporaryDirectory(prefix="pokepod-flash-identity-") as raw:
    temporary = Path(raw)
    authority_path = temporary / "authority.json"
    application_path = temporary / "application.json"
    chip_log = temporary / "chip-id.log"
    flash_log = temporary / "flash-id.log"
    verdict_path = temporary / "verdict.json"

    authority = {
        "schemaVersion": 1,
        "kind": "pokepod.flash-identity-authority",
        "profileId": "pokepod-amoled-1.8",
        "expected": {
            "chip": "ESP32-S3",
            "flashSizeBytes": 16 * 1024 * 1024,
            "boardVariant": "V2 CO5300/CST820",
            # Arduino reads the MAC bytes into a little-endian uint64_t.
            "deviceId": "pokepod-665544332211",
            "efuseMac": "11:22:33:44:55:66",
        },
    }
    application = {
        "status": "ok",
        "version": 2,
        "deviceId": "pokepod-665544332211",
        "displayName": "PokePod",
        "platform": "pokepod",
        "manufacturer": "PokeCapsule",
        "model": "V2 CO5300/CST820",
    }
    write_json(authority_path, authority)
    write_json(application_path, application)
    chip_log.write_text(
        "esptool v5.2.0\n"
        "Chip type: ESP32-S3 (QFN56) (revision v0.2)\n"
        "MAC: 11:22:33:44:55:66\n",
        encoding="utf-8",
    )
    flash_log.write_text(
        "Manufacturer: 20\nDevice: 4018\nDetected flash size: 16MB\n",
        encoding="utf-8",
    )

    passed = run(
        "evidence",
        "--authority", authority_path,
        "--application", application_path,
        "--chip-log", chip_log,
        "--flash-log", flash_log,
        "--output", verdict_path,
    )
    assert passed.returncode == 0, passed.stderr
    assert passed.stdout.strip() == "esp32s3-112233445566"
    verdict = json.loads(verdict_path.read_text(encoding="utf-8"))
    assert verdict["status"] == "pass"
    assert verdict["observed"] == {
        "applicationIdentityAvailable": True,
        "applicationDeviceId": "pokepod-665544332211",
        "applicationBoardVariant": "V2 CO5300/CST820",
        "boardVariantEvidence": "application-and-authority",
        "romChip": "ESP32-S3",
        "romEfuseMac": "11:22:33:44:55:66",
        "romFlashSizeBytes": 16 * 1024 * 1024,
    }

    # A damaged/absent application is recoverable only through the shell's
    # explicit ROM-port lane. ROM still must bind to authorized chip/MAC/size;
    # the board variant remains pre-authorized evidence, not a ROM claim.
    rescue_verdict = temporary / "rescue-verdict.json"
    rescued = run(
        "evidence",
        "--authority", authority_path,
        "--chip-log", chip_log,
        "--flash-log", flash_log,
        "--output", rescue_verdict,
    )
    assert rescued.returncode == 0, rescued.stderr
    observed = json.loads(rescue_verdict.read_text(encoding="utf-8"))["observed"]
    assert observed["applicationIdentityAvailable"] is False
    assert observed["applicationDeviceId"] is None
    assert observed["boardVariantEvidence"] == "authority-only-recovery"

    # Candidate selection rejects ambiguity and rejects one readable PokePod
    # whose application identity belongs to a different authorized device.
    second_application = temporary / "application-2.json"
    write_json(second_application, application)
    expect_failure(
        "flash_identity_application_response_count actual=2",
        "select-application", "--authority", authority_path,
        "--output", temporary / "selected.json",
        application_path, second_application,
    )
    wrong_identity = dict(application)
    wrong_identity["deviceId"] = "pokepod-000000000000"
    write_json(application_path, wrong_identity)
    expect_failure(
        "flash_identity_application_deviceId",
        "select-application", "--authority", authority_path,
        "--output", temporary / "selected.json", application_path,
    )
    write_json(application_path, application)

    bad_authority = json.loads(json.dumps(authority))
    del bad_authority["expected"]["efuseMac"]
    write_json(authority_path, bad_authority)
    expect_failure(
        "flash_identity_authority_efuse_mac",
        "authority", "--authority", authority_path,
        "--output", temporary / "summary.json",
    )

    expect_failure(
        "flash_identity_authority_invalid",
        "authority", "--authority", temporary / "missing-authority.json",
        "--output", temporary / "summary.json",
    )

    bad_authority = json.loads(json.dumps(authority))
    bad_authority["expected"]["boardVariant"] = "unknown"
    write_json(authority_path, bad_authority)
    expect_failure(
        "flash_identity_authority_board_variant",
        "authority", "--authority", authority_path,
        "--output", temporary / "summary.json",
    )

    write_json(authority_path, authority)
    bad_application = dict(application)
    bad_application["model"] = "V1 SH8601/FT3168"
    write_json(application_path, bad_application)
    expect_failure(
        "flash_identity_application_model",
        "evidence", "--authority", authority_path,
        "--application", application_path,
        "--chip-log", chip_log, "--flash-log", flash_log,
        "--output", verdict_path,
    )

    write_json(application_path, application)
    chip_log.write_text(
        "Chip type: ESP32-S2\nMAC: 11:22:33:44:55:66\n",
        encoding="utf-8",
    )
    expect_failure(
        "flash_identity_rom_chip",
        "evidence", "--authority", authority_path,
        "--application", application_path,
        "--chip-log", chip_log, "--flash-log", flash_log,
        "--output", verdict_path,
    )

    chip_log.write_text(
        "Chip is ESP32-S3\nMAC: aa:bb:cc:dd:ee:ff\n", encoding="utf-8"
    )
    expect_failure(
        "flash_identity_rom_efuse_mac_mismatch",
        "evidence", "--authority", authority_path,
        "--application", application_path,
        "--chip-log", chip_log, "--flash-log", flash_log,
        "--output", verdict_path,
    )

    chip_log.write_text(
        "Chip is ESP32-S3\nMAC: 11:22:33:44:55:66\n", encoding="utf-8"
    )
    flash_log.write_text("Detected flash size: 8MB\n", encoding="utf-8")
    expect_failure(
        "flash_identity_rom_flash_size_mismatch",
        "evidence", "--authority", authority_path,
        "--application", application_path,
        "--chip-log", chip_log, "--flash-log", flash_log,
        "--output", verdict_path,
    )

# The complete live identity verdict must precede backup and the first write.
evidence_call = 'DEVICE_KEY=$(python3 "$IDENTITY_VALIDATOR" evidence'
backup_call = 'python3 "$TRANSFER_SCRIPT" backup'
flash_call = 'python3 "$TRANSFER_SCRIPT" flash'
assert 'FAIL identity_authority_required' in FLASH
assert 'select-application' in FLASH
assert 'recovery_requires_explicit_rom_port' in FLASH
assert "CURRENT_PORTS=$(find_ports)" not in FLASH
assert evidence_call in FLASH and backup_call in FLASH and flash_call in FLASH
assert FLASH.index(evidence_call) < FLASH.index(backup_call) < FLASH.index(flash_call)
assert 'flash-id' in FLASH and 'chip-id' in FLASH

print("PASS flash_identity_gate")
