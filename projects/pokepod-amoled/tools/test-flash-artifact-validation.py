#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


TOOLS = Path(__file__).resolve().parent
VALIDATOR = TOOLS / "validate-flash-artifact.py"
POLICY_SOURCE = TOOLS / "flash-size-policy.py"
SPEC = importlib.util.spec_from_file_location("pokepod_flash_size_policy_test", POLICY_SOURCE)
assert SPEC and SPEC.loader
POLICY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POLICY)

IDENTITY_FORMAT = "<8sHH24s16s41sB3s41s65s"
IDENTITY_BYTES = struct.calcsize(IDENTITY_FORMAT)
IDENTITY_MAGIC = b"PKPDIMG2"
SOURCE_REVISION = "a" * 40
SOURCE_TREE = "b" * 40


def identity(source_dirty: bool = False) -> bytes:
    return struct.pack(
        IDENTITY_FORMAT,
        b"PKPDIMG2",
        2,
        IDENTITY_BYTES,
        b"PokePodAmoled\0".ljust(24, b"\0"),
        b"2.0.0\0".ljust(16, b"\0"),
        (SOURCE_REVISION + "\0").encode(),
        1 if source_dirty else 0,
        b"\0" * 3,
        (SOURCE_TREE + "\0").encode(),
        b"unknown\0".ljust(65, b"\0"),
    )


def write_binary(path: Path, size: int, fill: bytes, source_dirty: bool = False) -> None:
    assert size > IDENTITY_BYTES
    path.write_bytes(
        IDENTITY_MAGIC + fill * (size - IDENTITY_BYTES - len(IDENTITY_MAGIC)) +
        identity(source_dirty)
    )


def artifact(
    binary: Path,
    lane: str,
    dirty: bool,
    policy: dict[str, object],
    *,
    source_revision: str = SOURCE_REVISION,
    core_profile: str = "production",
    core_version: str = "3.3.8",
) -> dict[str, object]:
    payload = binary.read_bytes()
    return {
        "schemaVersion": 1,
        "kind": "hardmac.artifact",
        "lane": lane,
        "sourceRevision": source_revision,
        "sourceDirty": dirty,
        "sourceTree": SOURCE_TREE,
        "firmwareVersion": "2.0.0",
        "imageIdentity": {
            "magic": "PKPDIMG2",
            "schema": 2,
            "product": "PokePodAmoled",
            "firmwareVersion": "2.0.0",
            "sourceRevision": source_revision,
            "sourceTree": SOURCE_TREE,
            "sourceDirty": dirty,
            "appElfSha256": "unknown",
        },
        "binary": {
            "file": binary.name,
            "sizeBytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "flashOffset": "0x10000",
            "slotSizeBytes": 0x300000,
            "remainingBytes": policy["remainingBytes"],
            "usagePercent": policy["percent"],
            "resourceTier": policy["tier"],
        },
        "resourcePolicy": policy,
        "resourceReview": {
            "required": policy["tier"] in ("yellow", "orange"),
            "approved": False,
            "evidenceFile": "../../resource-review.json",
        },
        "toolchain": {
            "coreProfile": core_profile,
            "esp32ArduinoCore": core_version,
        },
    }


def validate(
    manifest: Path,
    binary: Path,
    lane: str,
    elf: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    command = [
        sys.executable,
        str(VALIDATOR),
        "--manifest",
        str(manifest),
        "--binary",
        str(binary),
        "--expected-lane",
        lane,
    ]
    if elf is not None:
        command.extend(("--elf", str(elf)))
    return subprocess.run(
        command,
        text=True,
        capture_output=True,
        check=False,
    )


with tempfile.TemporaryDirectory(prefix="pokepod-flash-artifact-") as raw:
    root = Path(raw)
    output = root / "work/output/fast"
    output.mkdir(parents=True)
    binary = output / "PokePodAmoled.ino.bin"
    write_binary(binary, 2_404_899, b"y", source_dirty=True)
    policy = POLICY.evaluate(binary.stat().st_size, 0x300000)
    manifest = output / "artifact.json"
    manifest.write_text(json.dumps(artifact(binary, "fast", True, policy)), encoding="utf-8")
    elf = output / "PokePodAmoled.ino.elf"
    elf.write_bytes(b"candidate-elf")
    manifest_payload = json.loads(manifest.read_text(encoding="utf-8"))
    manifest_payload["imageIdentity"]["appElfSha256"] = hashlib.sha256(
        elf.read_bytes()
    ).hexdigest()
    manifest.write_text(json.dumps(manifest_payload), encoding="utf-8")
    review = root / "work/resource-review.json"
    review.write_text(
        json.dumps(
            {
                "schema": "pokepod.resource-review.v1",
                "sourceRevision": SOURCE_REVISION,
                "binarySha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "programBytes": binary.stat().st_size,
                "tier": "yellow",
                "baseline": {
                    "commit": "base",
                    "programBytes": 2_338_335,
                    "deltaBytes": binary.stat().st_size - 2_338_335,
                },
                "largestSymbols": [{"sizeBytes": 100, "type": "T", "name": "main"}],
                "elf": {"file": "firmware.elf", "bytes": 3, "sha256": "1" * 64},
                "linkerMap": {"file": "firmware.map", "bytes": 3, "sha256": "2" * 64},
                "symbolSourceElfSha256": "1" * 64,
                "duplicateImplementationReview": {
                    "status": "pass",
                    "evidence": ["forbidden symbol rules passed"],
                    "forbiddenSymbolRegexes": ["legacyDuplicate"],
                    "matchedSymbols": [],
                },
                "nonessentialFeaturesFrozen": False,
                "sizeReductionReview": {"status": "not-required", "evidence": []},
            }
        ),
        encoding="utf-8",
    )
    assert validate(manifest, binary, "fast", elf).returncode == 0
    replaced_elf = output / "replaced.elf"
    replaced_elf.write_bytes(b"replaced-elf")
    replaced = validate(manifest, binary, "fast", replaced_elf)
    assert replaced.returncode != 0
    assert "artifact_image_identity_elf_sha256" in replaced.stderr

    missing_binary = output / "missing-identity.bin"
    missing_binary.write_bytes(b"m" * 512)
    missing_policy = POLICY.evaluate(missing_binary.stat().st_size, 0x300000)
    missing_manifest = output / "missing-identity.json"
    missing_manifest.write_text(
        json.dumps(artifact(missing_binary, "fast", False, missing_policy)),
        encoding="utf-8",
    )
    missing = validate(missing_manifest, missing_binary, "fast")
    assert missing.returncode != 0
    assert "image_identity_count actual=0" in missing.stderr

    duplicate_binary = output / "duplicate-identity.bin"
    duplicate_binary.write_bytes(b"d" * 512 + identity() + identity())
    duplicate_policy = POLICY.evaluate(duplicate_binary.stat().st_size, 0x300000)
    duplicate_manifest = output / "duplicate-identity.json"
    duplicate_manifest.write_text(
        json.dumps(artifact(duplicate_binary, "fast", False, duplicate_policy)),
        encoding="utf-8",
    )
    duplicate = validate(duplicate_manifest, duplicate_binary, "fast")
    assert duplicate.returncode != 0
    assert "image_identity_count actual=2" in duplicate.stderr

    red_binary = output / "red.bin"
    write_binary(red_binary, 2_700_000, b"r")
    forged_policy = POLICY.evaluate(2_350_000, 0x300000)
    forged_manifest = output / "red-artifact.json"
    forged_manifest.write_text(
        json.dumps(artifact(red_binary, "fast", False, forged_policy)), encoding="utf-8"
    )
    forged = validate(forged_manifest, red_binary, "fast")
    assert forged.returncode != 0
    assert "resource_policy_mismatch" in forged.stderr

    green_binary = output / "green.bin"
    write_binary(green_binary, 512, b"g", source_dirty=True)
    green_policy = POLICY.evaluate(green_binary.stat().st_size, 0x300000)
    dirty_release = output / "dirty-release.json"
    dirty_release.write_text(
        json.dumps(artifact(green_binary, "release", True, green_policy)), encoding="utf-8"
    )
    dirty = validate(dirty_release, green_binary, "release")
    assert dirty.returncode != 0
    assert "source_dirty" in dirty.stderr

    clean_binary = output / "clean.bin"
    write_binary(clean_binary, 512, b"c")
    clean_policy = POLICY.evaluate(clean_binary.stat().st_size, 0x300000)
    clean_release = output / "clean-release.json"
    clean_release.write_text(
        json.dumps(
            artifact(
                clean_binary,
                "release",
                False,
                clean_policy,
                source_revision="a" * 40,
            )
        ),
        encoding="utf-8",
    )
    assert validate(clean_release, clean_binary, "release").returncode == 0

    matrix_release = output / "matrix-release.json"
    matrix_release.write_text(
        json.dumps(
            artifact(
                clean_binary,
                "release",
                False,
                clean_policy,
                source_revision="a" * 40,
                core_profile="matrix",
                core_version="3.3.11",
            )
        ),
        encoding="utf-8",
    )
    matrix = validate(matrix_release, clean_binary, "release")
    assert matrix.returncode != 0
    assert "release_artifact_toolchain" in matrix.stderr

    wrong_offset_payload = artifact(binary, "fast", True, policy)
    wrong_offset_payload["binary"]["flashOffset"] = "0x0"
    wrong_offset = output / "wrong-offset.json"
    wrong_offset.write_text(json.dumps(wrong_offset_payload), encoding="utf-8")
    offset = validate(wrong_offset, binary, "fast")
    assert offset.returncode != 0
    assert "artifact_flash_offset" in offset.stderr

print("PASS test-flash-artifact-validation")
