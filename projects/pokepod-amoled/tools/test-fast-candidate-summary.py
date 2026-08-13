#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


SCRIPT = Path(__file__).with_name("write-fast-candidate-summary.py")


def file_evidence(path: Path) -> dict[str, object]:
    payload = path.read_bytes()
    return {
        "file": path.name,
        "bytes": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
    }


with tempfile.TemporaryDirectory(prefix="pokepod-fast-candidate-") as raw:
    root = Path(raw)
    binary = root / "PokePodAmoled.ino.bin"
    binary.write_bytes(b"firmware")
    elf = root / "PokePodAmoled.ino.elf"
    elf.write_bytes(b"elf")
    linker_map = root / "PokePodAmoled.ino.map"
    linker_map.write_bytes(b"map")
    build_log = root / "build-fast.log"
    build_log.write_text(
        "Sketch uses 8 bytes (0%) of program storage space. Maximum is 3145728 bytes.\n"
        "Global variables use 12 bytes (3%) of dynamic memory, leaving 388 bytes for local variables. Maximum is 400 bytes.\n",
        encoding="utf-8",
    )
    flash = {
        "schema": "pokepod.flash-size-policy.v1",
        "programBytes": 8,
        "slotBytes": 3145728,
        "remainingBytes": 3145720,
        "percent": 0.0003,
        "tier": "green",
        "releaseAllowed": True,
    }
    flash_path = root / "flash-resource.json"
    flash_path.write_text(json.dumps(flash), encoding="utf-8")
    artifact = {
        "kind": "hardmac.artifact",
        "lane": "fast",
        "sourceRevision": "candidate",
        "sourceDirty": False,
        "binary": {"sizeBytes": 8, "sha256": file_evidence(binary)["sha256"]},
        "toolchain": {"esp32ArduinoCore": "3.3.8"},
    }
    artifact_path = root / "artifact.json"
    artifact_path.write_text(json.dumps(artifact), encoding="utf-8")
    review = {
        "schema": "pokepod.resource-review.v1",
        "sourceRevision": "candidate",
        "binarySha256": file_evidence(binary)["sha256"],
        "programBytes": 8,
        "tier": "green",
        "baseline": {"commit": "base", "programBytes": 4, "deltaBytes": 4},
        "elf": file_evidence(elf),
        "linkerMap": file_evidence(linker_map),
        "largestSymbols": [{"sizeBytes": 4, "type": "T", "name": "main"}],
        "duplicateImplementationReview": {
            "status": "pass",
            "evidence": ["single implementation"],
            "forbiddenSymbolRegexes": ["legacyDuplicate"],
            "matchedSymbols": [],
        },
    }
    review_path = root / "resource-review.json"
    review_path.write_text(json.dumps(review), encoding="utf-8")
    output = root / "fast-candidate-summary.json"
    command = [
        sys.executable,
        str(SCRIPT),
        "--output", str(output),
        "--source-revision", "candidate",
        "--artifact", str(artifact_path),
        "--flash-resource", str(flash_path),
        "--resource-review", str(review_path),
        "--build-log", str(build_log),
        "--binary", str(binary),
        "--elf", str(elf),
        "--map", str(linker_map),
        "--baseline-commit", "base",
        "--baseline-program-bytes", "4",
        "--baseline-internal-globals", "10",
    ]
    subprocess.check_call(command)
    summary = json.loads(output.read_text(encoding="utf-8"))
    assert summary["delta"] == {"programBytes": 4, "internalGlobalBytes": 2}
    assert summary["internalMemory"]["maximumBytes"] == 400
    assert summary["binary"]["sha256"] == file_evidence(binary)["sha256"]
    assert summary["duplicateImplementationReview"]["status"] == "pass"

    dirty = dict(artifact)
    dirty["sourceDirty"] = True
    artifact_path.write_text(json.dumps(dirty), encoding="utf-8")
    rejected = subprocess.run(command, text=True, capture_output=True, check=False)
    assert rejected.returncode != 0
    assert "clean source tree" in rejected.stderr

print("PASS test-fast-candidate-summary")
