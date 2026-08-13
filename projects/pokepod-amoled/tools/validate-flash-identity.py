#!/usr/bin/env python3
"""Bind one authorized PokePod's Link identity to ROM identity evidence."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re


AUTHORITY_KIND = "pokepod.flash-identity-authority"
PROFILE_ID = "pokepod-amoled-1.8"
CHIP = "ESP32-S3"
FLASH_BYTES = 16 * 1024 * 1024
BOARD_VARIANTS = {"V1 SH8601/FT3168", "V2 CO5300/CST820"}


def fail(reason: str) -> None:
    raise SystemExit(f"FAIL flash_identity_{reason}")


def read_json(path: Path, label: str) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"{label}_invalid error={error}")
    if not isinstance(value, dict):
        fail(f"{label}_invalid")
    return value


def normalize_mac(value: object) -> str:
    if not isinstance(value, str):
        fail("authority_efuse_mac")
    compact = re.sub(r"[:-]", "", value.strip()).lower()
    if re.fullmatch(r"[0-9a-f]{12}", compact) is None:
        fail("authority_efuse_mac")
    return ":".join(compact[index:index + 2] for index in range(0, 12, 2))


def device_id_from_mac(mac: str) -> str:
    # ESP.getEfuseMac() receives the six network-order bytes in a little-endian
    # uint64_t. PokePod formats that integer, so its deviceId reverses the
    # conventional MAC byte order reported by esptool.
    octets = mac.split(":")
    return "pokepod-" + "".join(reversed(octets))


def read_authority(path: Path) -> dict[str, object]:
    value = read_json(path, "authority")
    if value.get("schemaVersion") != 1 or value.get("kind") != AUTHORITY_KIND:
        fail("authority_kind")
    if value.get("profileId") != PROFILE_ID:
        fail("authority_profile")
    expected = value.get("expected")
    if not isinstance(expected, dict):
        fail("authority_expected")
    mac = normalize_mac(expected.get("efuseMac"))
    normalized = {
        "chip": expected.get("chip"),
        "flashSizeBytes": expected.get("flashSizeBytes"),
        "boardVariant": expected.get("boardVariant"),
        "deviceId": expected.get("deviceId"),
        "efuseMac": mac,
    }
    if normalized["chip"] != CHIP:
        fail("authority_chip")
    if normalized["flashSizeBytes"] != FLASH_BYTES:
        fail("authority_flash_size")
    if normalized["boardVariant"] not in BOARD_VARIANTS:
        fail("authority_board_variant")
    if normalized["deviceId"] != device_id_from_mac(mac):
        fail("authority_device_id")
    return normalized


def validate_application(
    value: dict[str, object], expected: dict[str, object]
) -> None:
    required = {
        "status": "ok",
        "version": 2,
        "displayName": "PokePod",
        "platform": "pokepod",
        "manufacturer": "PokeCapsule",
        "deviceId": expected["deviceId"],
        "model": expected["boardVariant"],
    }
    for key, wanted in required.items():
        if value.get(key) != wanted:
            fail(f"application_{key}")


def capture_application(args: argparse.Namespace) -> int:
    try:
        spec = importlib.util.spec_from_file_location("pokepod_cdc_status", args.probe)
        if spec is None or spec.loader is None:
            fail("application_probe")
        probe = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(probe)
        result = probe.query(str(args.port), "identity", args.timeout)
    except SystemExit:
        raise
    except Exception as error:  # The transport probe defines its own errors.
        fail(f"application_query error={error}")
    if not isinstance(result, dict):
        fail("application_result")
    if args.authority is not None:
        validate_application(result, read_authority(args.authority))
    args.output.write_text(
        json.dumps(result, separators=(",", ":"), sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


def validate_application_file(args: argparse.Namespace) -> int:
    expected = read_authority(args.authority)
    validate_application(read_json(args.application, "application_identity"), expected)
    return 0


def select_application(args: argparse.Namespace) -> int:
    if len(args.candidates) != 1:
        fail(f"application_response_count actual={len(args.candidates)}")
    expected = read_authority(args.authority)
    value = read_json(args.candidates[0], "application_identity")
    validate_application(value, expected)
    args.output.write_text(
        json.dumps(value, separators=(",", ":"), sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


def rom_chip(text: str) -> str:
    if re.search(
        r"(?im)^\s*(?:Chip type|Chip is)\s*:?\s*ESP32-S3\b", text
    ) is None:
        fail("rom_chip")
    return CHIP


def rom_mac(text: str) -> str:
    match = re.search(
        r"(?im)^\s*MAC\s*:\s*((?:[0-9a-f]{2}:){5}[0-9a-f]{2})\s*$",
        text,
    )
    if match is None:
        fail("rom_efuse_mac_missing")
    return normalize_mac(match.group(1))


def rom_flash_bytes(text: str) -> int:
    match = re.search(
        r"(?im)^\s*Detected flash size\s*:\s*(\d+)\s*(KB|MB)\s*$", text
    )
    if match is None:
        fail("rom_flash_size_missing")
    unit = 1024 if match.group(2).upper() == "KB" else 1024 * 1024
    return int(match.group(1)) * unit


def write_authority_summary(args: argparse.Namespace) -> int:
    expected = read_authority(args.authority)
    args.output.write_text(
        json.dumps({
            "schemaVersion": 1,
            "kind": "pokepod.flash-identity-authority-summary",
            "authoritySha256": hashlib.sha256(args.authority.read_bytes()).hexdigest(),
            "expected": expected,
        }, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


def validate_evidence(args: argparse.Namespace) -> int:
    expected = read_authority(args.authority)
    application = None
    if args.application is not None:
        application = read_json(args.application, "application_identity")
        validate_application(application, expected)
    chip_text = args.chip_log.read_text(encoding="utf-8", errors="replace")
    flash_text = args.flash_log.read_text(encoding="utf-8", errors="replace")
    observed_chip = rom_chip(chip_text)
    observed_mac = rom_mac(chip_text)
    observed_flash_bytes = rom_flash_bytes(flash_text)
    if observed_mac != expected["efuseMac"]:
        fail("rom_efuse_mac_mismatch")
    if observed_flash_bytes != expected["flashSizeBytes"]:
        fail("rom_flash_size_mismatch")
    if (
        application is not None
        and device_id_from_mac(observed_mac) != application["deviceId"]
    ):
        fail("application_rom_rebind")
    observed_device_key = "esp32s3-" + observed_mac.replace(":", "")
    if (
        args.expected_device_key is not None
        and observed_device_key != args.expected_device_key
    ):
        fail("prewrite_device_key_mismatch")
    verdict = {
        "schemaVersion": 1,
        "kind": "pokepod.flash-identity-verdict",
        "status": "pass",
        "authoritySha256": hashlib.sha256(args.authority.read_bytes()).hexdigest(),
        "expected": expected,
        "observed": {
            "applicationIdentityAvailable": application is not None,
            "applicationDeviceId": (
                application["deviceId"] if application is not None else None
            ),
            "applicationBoardVariant": (
                application["model"] if application is not None else None
            ),
            "boardVariantEvidence": (
                "application-and-authority" if application is not None
                else "authority-only-recovery"
            ),
            "romChip": observed_chip,
            "romEfuseMac": observed_mac,
            "romFlashSizeBytes": observed_flash_bytes,
        },
    }
    args.output.write_text(
        json.dumps(verdict, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(observed_device_key)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    authority = subparsers.add_parser("authority")
    authority.add_argument("--authority", type=Path, required=True)
    authority.add_argument("--output", type=Path, required=True)
    authority.set_defaults(handler=write_authority_summary)

    capture = subparsers.add_parser("capture-application")
    capture.add_argument("--authority", type=Path)
    capture.add_argument("--probe", type=Path, required=True)
    capture.add_argument("--port", type=Path, required=True)
    capture.add_argument("--output", type=Path, required=True)
    capture.add_argument("--timeout", type=float, default=2.0)
    capture.set_defaults(handler=capture_application)

    application = subparsers.add_parser("application")
    application.add_argument("--authority", type=Path, required=True)
    application.add_argument("--application", type=Path, required=True)
    application.set_defaults(handler=validate_application_file)

    selection = subparsers.add_parser("select-application")
    selection.add_argument("--authority", type=Path, required=True)
    selection.add_argument("--output", type=Path, required=True)
    selection.add_argument("candidates", type=Path, nargs="+")
    selection.set_defaults(handler=select_application)

    evidence = subparsers.add_parser("evidence")
    evidence.add_argument("--authority", type=Path, required=True)
    evidence.add_argument("--application", type=Path)
    evidence.add_argument("--chip-log", type=Path, required=True)
    evidence.add_argument("--flash-log", type=Path, required=True)
    evidence.add_argument("--expected-device-key")
    evidence.add_argument("--output", type=Path, required=True)
    evidence.set_defaults(handler=validate_evidence)

    args = parser.parse_args()
    return args.handler(args)


if __name__ == "__main__":
    raise SystemExit(main())
