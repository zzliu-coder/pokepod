#!/usr/bin/env python3
"""Write a lane-bound Hard Mac artifact manifest for PokePod."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--lane", choices=("fast", "release"), required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--source-dirty", choices=("true", "false"), required=True)
    parser.add_argument("--build-input", required=True)
    parser.add_argument("--binary-sha256", required=True)
    parser.add_argument("--binary-size", type=int, required=True)
    parser.add_argument("--created-at", required=True)
    parser.add_argument("--fqbn", required=True)
    parser.add_argument("--core-version", required=True)
    parser.add_argument("--vendor-revision", required=True)
    args = parser.parse_args()

    manifest = {
        "schemaVersion": 1,
        "kind": "hardmac.artifact",
        "lane": args.lane,
        "sourceRevision": args.source_revision,
        "sourceDirty": args.source_dirty == "true",
        "buildInputSha256": args.build_input,
        "createdAt": args.created_at,
        "binary": {
            "file": "PokePodAmoled.ino.bin",
            "sizeBytes": args.binary_size,
            "sha256": args.binary_sha256,
        },
        "toolchain": {
            "fqbn": args.fqbn,
            "esp32ArduinoCore": args.core_version,
            "waveshareRevision": args.vendor_revision,
        },
    }
    Path(args.output).write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
