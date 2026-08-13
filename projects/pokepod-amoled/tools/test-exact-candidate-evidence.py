#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


TOOLS = Path(__file__).resolve().parent
SCRIPT = TOOLS / "verify-fast-candidate-evidence.py"
REVISION = "a" * 40
WRITER = TOOLS / "write-github-candidate-manifest.py"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


with tempfile.TemporaryDirectory(prefix="pokepod-exact-evidence-") as raw:
    root = Path(raw)
    paths = {
        name: root / name
        for name in (
            "firmware.bin", "firmware.elf", "firmware.map", "build.log",
            "artifact.json", "flash-resource.json", "resource-review.json",
            "summary.json",
        )
    }
    for index, path in enumerate(paths.values()):
        path.write_bytes(f"evidence-{index}".encode())
    paths["artifact.json"].write_text(
        json.dumps({"sourceRevision": REVISION, "sourceDirty": False}),
        encoding="utf-8",
    )
    paths["flash-resource.json"].write_text(
        json.dumps({
            "schema": "pokepod.flash-size-policy.v1",
            "releaseAllowed": True,
        }),
        encoding="utf-8",
    )
    paths["resource-review.json"].write_text(
        json.dumps({"sourceRevision": REVISION}), encoding="utf-8"
    )
    paths["summary.json"].write_text(
        json.dumps({"sourceRevision": REVISION}), encoding="utf-8"
    )
    sha_manifest = root / "candidate-sha256.txt"
    sha_manifest.write_text(
        "".join(f"{digest(path)}  {path}\n" for path in paths.values()),
        encoding="utf-8",
    )
    candidate_manifest = root / "candidate-manifest.json"
    writer_command = [
        sys.executable, str(WRITER), "--output", str(candidate_manifest),
        "--source-revision", REVISION,
    ]
    for path in paths.values():
        writer_command.extend(("--file", str(path)))
    subprocess.check_call(writer_command)
    command = [
        sys.executable, str(SCRIPT), "--source-revision", REVISION,
        "--binary", str(paths["firmware.bin"]),
        "--elf", str(paths["firmware.elf"]),
        "--map", str(paths["firmware.map"]),
        "--build-log", str(paths["build.log"]),
        "--artifact", str(paths["artifact.json"]),
        "--flash-resource", str(paths["flash-resource.json"]),
        "--resource-review", str(paths["resource-review.json"]),
        "--summary", str(paths["summary.json"]),
        "--sha256-manifest", str(sha_manifest),
        "--candidate-manifest", str(candidate_manifest),
    ]
    passed = subprocess.run(command, text=True, capture_output=True, check=False)
    assert passed.returncode == 0, passed.stdout + passed.stderr

    bad_sha = command.copy()
    bad_sha[bad_sha.index(REVISION)] = "b" * 40
    rejected = subprocess.run(bad_sha, text=True, capture_output=True, check=False)
    assert rejected.returncode != 0
    assert "SHA mismatch" in rejected.stderr

    paths["firmware.map"].unlink()
    missing = subprocess.run(command, text=True, capture_output=True, check=False)
    assert missing.returncode != 0
    assert "missing or empty" in missing.stderr

print("PASS test-exact-candidate-evidence")
