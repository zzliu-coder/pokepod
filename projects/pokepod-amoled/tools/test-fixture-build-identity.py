#!/usr/bin/env python3
"""Exercise artifact and fixture exact-build identity as a closed contract."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FIXTURE_DIR = ROOT / "fixture"
sys.path.insert(0, str(FIXTURE_DIR))


def import_file(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


fixture = import_file("pokepod_fixture", FIXTURE_DIR / "pokepod-fixture.py")
policy = import_file("flash_size_policy", ROOT / "tools" / "flash-size-policy.py")


def main() -> int:
    revision = "1" * 40
    version = "2.0.0"
    elf_sha = bytes(range(32))
    payload = bytearray(b"\xff" * 4096)
    payload[32:36] = (0xABCD5432).to_bytes(4, "little")
    marker = struct.pack(
        "<8sHH24s16s41sB3s",
        b"PKPDIMG2", 1, 97,
        b"PokePodAmoled\0".ljust(24, b"\0"),
        (version.encode() + b"\0").ljust(16, b"\0"),
        (revision.encode() + b"\0").ljust(41, b"\0"),
        0, b"\0\0\0",
    )
    payload[512:512 + len(marker)] = marker
    payload[176:208] = elf_sha
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        binary_path = directory / "PokePodAmoled.ino.bin"
        binary_path.write_bytes(payload)
        sha = hashlib.sha256(payload).hexdigest()
        resource = policy.evaluate(len(payload), 0x300000)
        manifest = {
            "schemaVersion": 1,
            "kind": "hardmac.artifact",
            "lane": "fast",
            "sourceRevision": revision,
            "sourceDirty": False,
            "firmwareVersion": version,
            "binary": {
                "file": binary_path.name,
                "sizeBytes": len(payload),
                "sha256": sha,
                "flashOffset": "0x10000",
                "slotSizeBytes": 0x300000,
                "remainingBytes": resource["remainingBytes"],
                "usagePercent": resource["percent"],
                "resourceTier": resource["tier"],
            },
            "resourcePolicy": resource,
            "resourceReview": {"required": False, "approved": False},
            "toolchain": {
                "coreProfile": "production",
                "esp32ArduinoCore": "3.3.8",
            },
        }
        (directory / "artifact.json").write_text(
            json.dumps(manifest), encoding="utf-8"
        )
        expected = fixture.expected_build_from_artifact(binary_path)
        assert expected == {
            "sourceRevision": revision,
            "firmwareVersion": version,
            "appElfSha256": elf_sha.hex(),
            "binarySha256": sha,
        }
        fixture.require_expected_build({
            "sourceRevision": revision,
            "firmwareVersion": version,
            "appElfSha256": elf_sha.hex(),
            "sourceDirty": False,
            "runningPartition": "app0",
        }, expected)
        try:
            fixture.require_expected_build({
                "sourceRevision": "2" * 40,
                "firmwareVersion": version,
                "appElfSha256": elf_sha.hex(),
                "sourceDirty": False,
                "runningPartition": "app0",
            }, expected)
        except RuntimeError:
            pass
        else:
            raise AssertionError("mismatched source revision must be rejected")
    print("PASS fixture_exact_build_identity")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
