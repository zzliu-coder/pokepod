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
    sources["build.log"].write_text(
        "Sketch uses 9 bytes (0%) of program storage space. Maximum is 3145728 bytes.\n"
        "Global variables use 10 bytes (10%) of dynamic memory, leaving 90 bytes for local variables. Maximum is 100 bytes.\n",
        encoding="utf-8")
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
    def payload_evidence(path: Path, filename: str) -> dict[str, object]:
        value = evidence(path)
        value["file"] = filename
        return value
    sources["resource-review.json"].write_text(json.dumps({
        "schema": "pokepod.resource-review.v1", "sourceRevision": REVISION,
        "binarySha256": binary_sha, "programBytes": binary_bytes,
        "tier": "green", "baseline": {"commit": "base",
        "programBytes": 1, "deltaBytes": binary_bytes - 1},
        "elf": payload_evidence(sources["firmware.elf"], "PokePodAmoled.ino.elf"),
        "linkerMap": payload_evidence(sources["firmware.map"], "PokePodAmoled.ino.map"),
        "symbolSourceElfSha256": evidence(sources["firmware.elf"])["sha256"],
        "duplicateImplementationReview": {"status": "pass", "matchedSymbols": []},
    }), encoding="utf-8")
    sources["summary.json"].write_text(json.dumps({
        "schema": "pokepod.fast-candidate-evidence.v1",
        "sourceRevision": REVISION, "sourceClean": True,
        "resourceReviewApproved": False,
        "binary": {"bytes": binary_bytes, "sha256": binary_sha},
        "elf": payload_evidence(sources["firmware.elf"], "PokePodAmoled.ino.elf"),
        "linkerMap": payload_evidence(sources["firmware.map"], "PokePodAmoled.ino.map"),
        "buildLog": payload_evidence(sources["build.log"], "build-fast.log"),
        "flash": flash, "delta": {"programBytes": binary_bytes - 1,
                                    "internalGlobalBytes": 2},
        "linkedProgram": {"bytes": 9,
                           "imagePackagingBytes": binary_bytes - 9},
        "internalMemory": {"globalBytes": 10, "remainingBytes": 90,
                            "maximumBytes": 100},
        "baseline": {"commit": "base", "programBytes": 1,
                      "deltaBytes": binary_bytes - 1,
                      "internalGlobalBytes": 8},
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

    def resign(candidate: Path, filename: str) -> None:
        manifest_path = candidate / "candidate-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        payload = candidate / filename
        for entry in manifest["files"]:
            if entry["file"] == filename:
                entry["bytes"] = payload.stat().st_size
                entry["sha256"] = hashlib.sha256(payload.read_bytes()).hexdigest()
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n",
                                 encoding="utf-8")
        (candidate / "candidate-sha256.txt").write_text(
            "".join(
                f"{hashlib.sha256((candidate / name).read_bytes()).hexdigest()}  {name}\n"
                for name in (
                    "PokePodAmoled.ino.bin", "PokePodAmoled.ino.elf",
                    "PokePodAmoled.ino.map", "build-fast.log",
                    "artifact.json", "flash-resource.json",
                    "resource-review.json", "fast-candidate-summary.json",
                    "candidate-manifest.json")),
            encoding="utf-8")

    for filename, needle in (
        ("PokePodAmoled.ino.elf", "resource review ELF evidence"),
        ("PokePodAmoled.ino.map", "resource review linker map evidence"),
        ("build-fast.log", "summary build log evidence"),
    ):
        adversarial = root / ("replaced-" + filename.replace(".", "-"))
        stage(adversarial)
        with (adversarial / filename).open("ab") as stream:
            stream.write(b"replacement")
        resign(adversarial, filename)
        rejected = verify(adversarial)
        assert rejected.returncode != 0, filename
        assert needle in rejected.stderr, rejected.stderr

print("PASS test-exact-candidate-evidence")
