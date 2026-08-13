#!/usr/bin/env python3
"""Verify one exact-SHA Fast candidate evidence directory after generation."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--map", dest="linker_map", type=Path, required=True)
    parser.add_argument("--build-log", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--flash-resource", type=Path, required=True)
    parser.add_argument("--resource-review", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--sha256-manifest", type=Path, required=True)
    parser.add_argument("--candidate-manifest", type=Path, required=True)
    args = parser.parse_args()

    revision = args.source_revision.lower()
    require(len(revision) == 40 and all(c in "0123456789abcdef" for c in revision),
            "source revision must be one full Git SHA-1")
    required = (
        args.binary, args.elf, args.linker_map, args.build_log, args.artifact,
        args.flash_resource, args.resource_review, args.summary,
        args.sha256_manifest, args.candidate_manifest,
    )
    for path in required:
        require(path.is_file() and path.stat().st_size > 0,
                f"candidate evidence missing or empty: {path}")

    artifact = json.loads(args.artifact.read_text(encoding="utf-8"))
    flash = json.loads(args.flash_resource.read_text(encoding="utf-8"))
    review = json.loads(args.resource_review.read_text(encoding="utf-8"))
    summary = json.loads(args.summary.read_text(encoding="utf-8"))
    candidate = json.loads(args.candidate_manifest.read_text(encoding="utf-8"))
    require(artifact.get("sourceRevision") == revision, "artifact SHA mismatch")
    require(artifact.get("sourceDirty") is False, "artifact source is dirty")
    require(flash.get("schema") == "pokepod.flash-size-policy.v1",
            "flash resource schema mismatch")
    require(flash.get("releaseAllowed") is True,
            "flash resource policy blocks this candidate")
    require(review.get("sourceRevision") == revision, "resource review SHA mismatch")
    require(summary.get("sourceRevision") == revision, "summary SHA mismatch")
    require(candidate.get("sourceRevision") == revision,
            "candidate manifest SHA mismatch")
    require(candidate.get("sourceClean") is True, "candidate manifest source is dirty")
    require(candidate.get("status") == "verified", "candidate manifest is not verified")

    expected_paths = {path.resolve(): sha256(path) for path in required[:-2]}
    manifest_lines = args.sha256_manifest.read_text(encoding="utf-8").splitlines()
    observed: dict[Path, str] = {}
    for line in manifest_lines:
        checksum, separator, raw_path = line.partition("  ")
        require(bool(separator), f"invalid SHA-256 manifest line: {line}")
        require(len(checksum) == 64 and all(c in "0123456789abcdef" for c in checksum),
                f"invalid SHA-256 digest: {checksum}")
        observed[Path(raw_path).resolve()] = checksum
    require(observed == expected_paths, "SHA-256 manifest file set or digest mismatch")

    candidate_files = candidate.get("files")
    require(isinstance(candidate_files, list), "candidate manifest files missing")
    by_name = {entry.get("file"): entry for entry in candidate_files
               if isinstance(entry, dict)}
    for path in required[:-2]:
        entry = by_name.get(path.name)
        require(isinstance(entry, dict), f"candidate manifest omits {path.name}")
        require(entry.get("bytes") == path.stat().st_size, f"size mismatch: {path.name}")
        require(entry.get("sha256") == sha256(path), f"digest mismatch: {path.name}")

    print(f"PASS exact_fast_candidate_evidence ({revision})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
