#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
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


def artifact(binary: Path, lane: str, dirty: bool, policy: dict[str, object]) -> dict[str, object]:
    payload = binary.read_bytes()
    return {
        "schemaVersion": 1,
        "kind": "hardmac.artifact",
        "lane": lane,
        "sourceRevision": "abc",
        "sourceDirty": dirty,
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
    }


def validate(manifest: Path, binary: Path, lane: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(VALIDATOR),
            "--manifest",
            str(manifest),
            "--binary",
            str(binary),
            "--expected-lane",
            lane,
        ],
        text=True,
        capture_output=True,
        check=False,
    )


with tempfile.TemporaryDirectory(prefix="pokepod-flash-artifact-") as raw:
    root = Path(raw)
    output = root / "work/output/fast"
    output.mkdir(parents=True)
    binary = output / "PokePodAmoled.ino.bin"
    binary.write_bytes(b"y" * 2_404_899)
    policy = POLICY.evaluate(binary.stat().st_size, 0x300000)
    manifest = output / "artifact.json"
    manifest.write_text(json.dumps(artifact(binary, "fast", True, policy)), encoding="utf-8")
    review = root / "work/resource-review.json"
    review.write_text(
        json.dumps(
            {
                "schema": "pokepod.resource-review.v1",
                "sourceRevision": "abc",
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
    assert validate(manifest, binary, "fast").returncode == 0

    red_binary = output / "red.bin"
    red_binary.write_bytes(b"r" * 2_700_000)
    forged_policy = POLICY.evaluate(2_350_000, 0x300000)
    forged_manifest = output / "red-artifact.json"
    forged_manifest.write_text(
        json.dumps(artifact(red_binary, "fast", True, forged_policy)), encoding="utf-8"
    )
    forged = validate(forged_manifest, red_binary, "fast")
    assert forged.returncode != 0
    assert "resource_policy_mismatch" in forged.stderr

    green_binary = output / "green.bin"
    green_binary.write_bytes(b"g" * 128)
    green_policy = POLICY.evaluate(green_binary.stat().st_size, 0x300000)
    dirty_release = output / "dirty-release.json"
    dirty_release.write_text(
        json.dumps(artifact(green_binary, "release", True, green_policy)), encoding="utf-8"
    )
    dirty = validate(dirty_release, green_binary, "release")
    assert dirty.returncode != 0
    assert "source_dirty" in dirty.stderr

print("PASS test-flash-artifact-validation")
