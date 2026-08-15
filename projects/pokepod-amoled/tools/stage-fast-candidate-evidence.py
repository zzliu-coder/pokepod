#!/usr/bin/env python3
"""Create one fresh, closed Fast-candidate directory for artifact upload."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil


PAYLOAD_ARGUMENTS = (
    ("binary", "PokePodAmoled.ino.bin"),
    ("elf", "PokePodAmoled.ino.elf"),
    ("linker_map", "PokePodAmoled.ino.map"),
    ("build_log", "build-fast.log"),
    ("artifact", "artifact.json"),
    ("flash_resource", "flash-resource.json"),
    ("resource_review", "resource-review.json"),
    ("summary", "fast-candidate-summary.json"),
)
MANIFEST_NAME = "candidate-manifest.json"
CHECKSUM_NAME = "candidate-sha256.txt"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_revision(value: str) -> str:
    revision = value.lower()
    if len(revision) != 40 or any(c not in "0123456789abcdef" for c in revision):
        raise ValueError("source revision must be one full Git SHA-1")
    return revision


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-revision", required=True)
    for argument, _ in PAYLOAD_ARGUMENTS:
        option = "map" if argument == "linker_map" else argument.replace("_", "-")
        parser.add_argument(f"--{option}", dest=argument, type=Path, required=True)
    args = parser.parse_args()

    revision = require_revision(args.source_revision)
    output = args.output
    if output.exists() or output.is_symlink():
        raise ValueError(f"candidate directory must be fresh: {output}")
    output.mkdir(parents=True)

    staged: list[Path] = []
    for argument, filename in PAYLOAD_ARGUMENTS:
        source = getattr(args, argument)
        if not source.is_file() or source.stat().st_size == 0:
            raise ValueError(f"candidate evidence missing or empty: {source}")
        destination = output / filename
        shutil.copyfile(source, destination)
        staged.append(destination)

    records = [
        {
            "file": path.name,
            "bytes": path.stat().st_size,
            "sha256": sha256(path),
        }
        for path in staged
    ]
    manifest = output / MANIFEST_NAME
    manifest.write_text(
        json.dumps(
            {
                "schema": "pokepod.github-fast-candidate.v2",
                "sourceRevision": revision,
                "sourceClean": True,
                # The verifier derives the final status only after checking
                # cross-file binary, policy and review semantics.
                "status": "pending",
                "resourceReviewApproved": json.loads(
                    args.artifact.read_text(encoding="utf-8")
                ).get("resourceReview", {}).get("approved"),
                "files": records,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    checksum_manifest = output / CHECKSUM_NAME
    checksum_manifest.write_text(
        "".join(f"{sha256(path)}  {path.name}\n" for path in (*staged, manifest)),
        encoding="utf-8",
    )
    print(f"STAGED exact_fast_candidate_evidence ({revision}) -> {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
