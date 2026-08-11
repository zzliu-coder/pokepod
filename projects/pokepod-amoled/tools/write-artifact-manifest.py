#!/usr/bin/env python3
"""Write a lane-bound Hard Mac artifact manifest for PokePod."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re


def read_firmware_version(header: Path) -> str:
    source = header.read_text(encoding="utf-8")
    match = re.search(
        r'constexpr\s+const\s+char\s+kFirmwareVersion\[\]\s*=\s*"([^"]+)"\s*;',
        source,
    )
    if match is None:
        raise ValueError(f"firmware version constant missing: {header}")
    return match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--lane", choices=("fast", "release"), required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--source-dirty", choices=("true", "false"), required=True)
    parser.add_argument("--build-input", required=True)
    parser.add_argument("--binary-sha256", required=True)
    parser.add_argument("--binary-size", type=int, required=True)
    parser.add_argument("--flash-policy", required=True)
    parser.add_argument("--resource-review-approved", choices=("true", "false"), required=True)
    parser.add_argument("--created-at", required=True)
    parser.add_argument("--fqbn", required=True)
    parser.add_argument("--core-version", required=True)
    parser.add_argument("--core-profile", choices=("production", "matrix"), required=True)
    parser.add_argument("--app-offset", required=True)
    parser.add_argument("--vendor-revision", required=True)
    parser.add_argument("--firmware-version-header", required=True)
    args = parser.parse_args()

    firmware_version = read_firmware_version(Path(args.firmware_version_header))
    flash_policy = json.loads(Path(args.flash_policy).read_text(encoding="utf-8"))
    if flash_policy.get("schema") != "pokepod.flash-size-policy.v1":
        raise ValueError("invalid flash size policy report")
    if flash_policy.get("programBytes") != args.binary_size:
        raise ValueError("flash size policy does not match firmware binary")

    manifest = {
        "schemaVersion": 1,
        "kind": "hardmac.artifact",
        "lane": args.lane,
        "sourceRevision": args.source_revision,
        "sourceDirty": args.source_dirty == "true",
        "firmwareVersion": firmware_version,
        "buildInputSha256": args.build_input,
        "createdAt": args.created_at,
        "binary": {
            "file": "PokePodAmoled.ino.bin",
            "sizeBytes": args.binary_size,
            "sha256": args.binary_sha256,
            "flashOffset": args.app_offset,
            "slotSizeBytes": flash_policy["slotBytes"],
            "remainingBytes": flash_policy["remainingBytes"],
            "usagePercent": flash_policy["percent"],
            "resourceTier": flash_policy["tier"],
        },
        "resourcePolicy": flash_policy,
        "resourceReview": {
            "required": flash_policy["tier"] in ("yellow", "orange"),
            "approved": args.resource_review_approved == "true",
            "evidenceFile": "../../resource-review.json",
        },
        "toolchain": {
            "fqbn": args.fqbn,
            "esp32ArduinoCore": args.core_version,
            "coreProfile": args.core_profile,
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
