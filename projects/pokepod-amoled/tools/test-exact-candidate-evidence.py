#!/usr/bin/env python3
from __future__ import annotations

import json
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile


TOOLS = Path(__file__).resolve().parent
STAGER = TOOLS / "stage-fast-candidate-evidence.py"
VERIFIER = TOOLS / "verify-fast-candidate-evidence.py"
REVISION = "a" * 40


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


with tempfile.TemporaryDirectory(prefix="pokepod-exact-evidence-") as raw:
    root = Path(raw)
    sources = {
        name: root / name
        for name in (
            "firmware.bin",
            "firmware.elf",
            "firmware.map",
            "build.log",
            "artifact.json",
            "flash-resource.json",
            "resource-review.json",
            "summary.json",
        )
    }
    for index, path in enumerate(sources.values()):
        path.write_bytes(f"evidence-{index}".encode())
    binary_bytes = sources["firmware.bin"].stat().st_size
    binary_sha = hashlib.sha256(sources["firmware.bin"].read_bytes()).hexdigest()
    flash = {
        "schema": "pokepod.flash-size-policy.v1",
        "programBytes": binary_bytes,
        "slotBytes": 3145728,
        "remainingBytes": 3145728 - binary_bytes,
        "percent": round(binary_bytes * 100 / 3145728, 4),
        "tier": "green",
        "releaseAllowed": True,
        "thresholds": {"greenBelowPercent": 75, "yellowBelowPercent": 80,
                       "orangeBelowPercent": 85, "redAtOrAbovePercent": 85},
    }
    sources["flash-resource.json"].write_text(json.dumps(flash), encoding="utf-8")
    sources["artifact.json"].write_text(json.dumps({
        "schemaVersion": 1, "kind": "hardmac.artifact", "lane": "fast",
        "sourceRevision": REVISION, "sourceDirty": False,
        "binary": {"file": "firmware.bin", "sizeBytes": binary_bytes,
                    "sha256": binary_sha, "flashOffset": "0x10000",
                    "slotSizeBytes": 3145728,
                    "remainingBytes": flash["remainingBytes"],
                    "usagePercent": flash["percent"],
                    "resourceTier": flash["tier"]},
        "resourcePolicy": flash,
        "resourceReview": {"required": False, "approved": False},
    }), encoding="utf-8")
    def evidence(path: Path) -> dict[str, object]:
        data = path.read_bytes()
        return {"file": path.name, "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest()}
    sources["resource-review.json"].write_text(json.dumps({
        "schema": "pokepod.resource-review.v1", "sourceRevision": REVISION,
        "binarySha256": binary_sha, "programBytes": binary_bytes,
        "tier": "green", "baseline": {"commit": "base",
        "programBytes": 1, "deltaBytes": binary_bytes - 1},
        "elf": evidence(sources["firmware.elf"]),
        "linkerMap": evidence(sources["firmware.map"]),
        "duplicateImplementationReview": {"status": "pass", "matchedSymbols": []},
    }), encoding="utf-8")
    sources["summary.json"].write_text(json.dumps({
        "schema": "pokepod.fast-candidate-evidence.v1",
        "sourceRevision": REVISION, "sourceClean": True,
        "resourceReviewApproved": False,
        "binary": {"bytes": binary_bytes, "sha256": binary_sha},
        "flash": flash, "delta": {"programBytes": binary_bytes - 1},
        "artifactManifest": evidence(sources["artifact.json"]),
        "resourceReview": evidence(sources["resource-review.json"]),
    }), encoding="utf-8")

    def stage(candidate: Path) -> None:
        command = [
            sys.executable,
            str(STAGER),
            "--output",
            str(candidate),
            "--source-revision",
            REVISION,
            "--binary",
            str(sources["firmware.bin"]),
            "--elf",
            str(sources["firmware.elf"]),
            "--map",
            str(sources["firmware.map"]),
            "--build-log",
            str(sources["build.log"]),
            "--artifact",
            str(sources["artifact.json"]),
            "--flash-resource",
            str(sources["flash-resource.json"]),
            "--resource-review",
            str(sources["resource-review.json"]),
            "--summary",
            str(sources["summary.json"]),
        ]
        result = run(command)
        assert result.returncode == 0, result.stdout + result.stderr

    def verify(candidate: Path, revision: str = REVISION) -> subprocess.CompletedProcess[str]:
        return run(
            [
                sys.executable,
                str(VERIFIER),
                "--source-revision",
                revision,
                "--candidate-dir",
                str(candidate),
            ]
        )

    valid = root / "valid-candidate"
    stage(valid)
    passed = verify(valid)
    assert passed.returncode == 0, passed.stdout + passed.stderr

    bad_sha = verify(valid, "b" * 40)
    assert bad_sha.returncode != 0
    assert "SHA mismatch" in bad_sha.stderr

    mutated = root / "mutated-after-manifest"
    stage(mutated)
    with (mutated / "PokePodAmoled.ino.bin").open("ab") as stream:
        stream.write(b"post-manifest mutation")
    rejected_mutation = verify(mutated)
    assert rejected_mutation.returncode != 0
    assert rejected_mutation.returncode != 0

    unexpected = root / "unexpected-flashable-sibling"
    stage(unexpected)
    (unexpected / "merged.bin").write_bytes(b"unmanifested flash image")
    rejected_extra = verify(unexpected)
    assert rejected_extra.returncode != 0
    assert "file set mismatch" in rejected_extra.stderr
    assert "merged.bin" in rejected_extra.stderr

    missing = root / "missing-payload"
    stage(missing)
    (missing / "PokePodAmoled.ino.map").unlink()
    rejected_missing = verify(missing)
    assert rejected_missing.returncode != 0
    assert "file set mismatch" in rejected_missing.stderr

print("PASS test-exact-candidate-evidence")
