#!/usr/bin/env python3
from __future__ import annotations

import json
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
    sources["artifact.json"].write_text(
        json.dumps({"sourceRevision": REVISION, "sourceDirty": False}),
        encoding="utf-8",
    )
    sources["flash-resource.json"].write_text(
        json.dumps(
            {"schema": "pokepod.flash-size-policy.v1", "releaseAllowed": True}
        ),
        encoding="utf-8",
    )
    sources["resource-review.json"].write_text(
        json.dumps({"sourceRevision": REVISION}), encoding="utf-8"
    )
    sources["summary.json"].write_text(
        json.dumps({"sourceRevision": REVISION}), encoding="utf-8"
    )

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
    assert "digest mismatch" in rejected_mutation.stderr

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
