#!/usr/bin/env python3
"""Bind one clean Fast build to flash, RAM, ELF, map and symbol evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


MEMORY_RE = re.compile(
    r"Global variables use\s+([0-9,]+)\s+bytes\s+\(([0-9.]+)%\)\s+"
    r"of dynamic memory, leaving\s+([0-9,]+)\s+bytes.*?Maximum is\s+([0-9,]+)\s+bytes",
    re.IGNORECASE,
)
PROGRAM_RE = re.compile(
    r"Sketch uses\s+([0-9,]+)\s+bytes\s+\(([0-9.]+)%\)\s+"
    r"of program storage space.*?Maximum is\s+([0-9,]+)\s+bytes",
    re.IGNORECASE,
)


def non_negative(value: str) -> int:
    parsed = int(value, 0)
    if parsed < 0:
        raise argparse.ArgumentTypeError("value must be non-negative")
    return parsed


def read_json(path: Path) -> dict[str, object]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected JSON object: {path}")
    return value


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def evidence(path: Path) -> dict[str, object]:
    return {"file": path.name, "bytes": path.stat().st_size, "sha256": digest(path)}


def parse_number(raw: str) -> int:
    return int(raw.replace(",", ""))


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--flash-resource", type=Path, required=True)
    parser.add_argument("--resource-review", type=Path, required=True)
    parser.add_argument("--build-log", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--map", dest="linker_map", type=Path, required=True)
    parser.add_argument("--baseline-commit", required=True)
    parser.add_argument("--baseline-program-bytes", type=non_negative, required=True)
    parser.add_argument("--baseline-internal-globals", type=non_negative, required=True)
    args = parser.parse_args()

    artifact = read_json(args.artifact)
    flash = read_json(args.flash_resource)
    review = read_json(args.resource_review)
    log = args.build_log.read_text(encoding="utf-8", errors="replace")
    memory_match = MEMORY_RE.search(log)
    program_match = PROGRAM_RE.search(log)
    require(memory_match is not None, "Arduino internal-memory summary missing from build log")
    require(program_match is not None, "Arduino program-size summary missing from build log")
    assert memory_match is not None and program_match is not None

    binary_evidence = evidence(args.binary)
    elf_evidence = evidence(args.elf)
    map_evidence = evidence(args.linker_map)
    binary_manifest = artifact.get("binary")
    require(isinstance(binary_manifest, dict), "artifact binary object missing")
    assert isinstance(binary_manifest, dict)
    require(artifact.get("kind") == "hardmac.artifact", "invalid artifact kind")
    require(artifact.get("lane") == "fast", "candidate evidence requires Fast lane")
    require(artifact.get("sourceRevision") == args.source_revision, "artifact source revision mismatch")
    require(artifact.get("sourceDirty") is False, "Fast candidate must come from a clean source tree")
    require(binary_manifest.get("sizeBytes") == binary_evidence["bytes"], "binary size mismatch")
    require(binary_manifest.get("sha256") == binary_evidence["sha256"], "binary digest mismatch")
    require(flash.get("schema") == "pokepod.flash-size-policy.v1", "invalid flash policy")
    require(flash.get("programBytes") == binary_evidence["bytes"], "flash policy binary mismatch")
    require(review.get("schema") == "pokepod.resource-review.v1", "invalid resource review")
    require(review.get("sourceRevision") == args.source_revision, "review source revision mismatch")
    require(review.get("binarySha256") == binary_evidence["sha256"], "review binary digest mismatch")
    require(review.get("programBytes") == binary_evidence["bytes"], "review binary size mismatch")
    require(review.get("tier") == flash.get("tier"), "resource tier mismatch")
    require(review.get("elf") == elf_evidence, "ELF evidence mismatch")
    require(review.get("linkerMap") == map_evidence, "linker map evidence mismatch")
    duplicate = review.get("duplicateImplementationReview")
    require(isinstance(duplicate, dict) and duplicate.get("status") == "pass", "duplicate review missing")
    require(duplicate.get("matchedSymbols") == [], "forbidden duplicate symbols present")
    baseline = review.get("baseline")
    require(isinstance(baseline, dict), "resource baseline missing")
    assert isinstance(baseline, dict)
    require(baseline.get("commit") == args.baseline_commit, "baseline commit mismatch")
    require(baseline.get("programBytes") == args.baseline_program_bytes, "baseline program size mismatch")

    log_program_bytes = parse_number(program_match.group(1))
    internal_globals = parse_number(memory_match.group(1))
    internal_remaining = parse_number(memory_match.group(3))
    internal_maximum = parse_number(memory_match.group(4))
    image_packaging_bytes = int(binary_evidence["bytes"]) - log_program_bytes
    require(image_packaging_bytes >= 0, "binary is smaller than linked program image")
    # Arduino's summary measures linked program segments. The flashable ESP
    # image adds its image header, segment headers, checksum and alignment.
    # Keep both facts and reject an unexpectedly large packaging gap while
    # continuing to apply release thresholds to the exact .bin byte count.
    require(image_packaging_bytes <= 4096, "binary packaging overhead is unexpectedly large")
    require(internal_globals + internal_remaining == internal_maximum, "build log RAM arithmetic mismatch")

    summary = {
        "schema": "pokepod.fast-candidate-evidence.v1",
        "sourceRevision": args.source_revision,
        "sourceClean": True,
        "lane": "fast",
        "resourceReviewApproved": bool(
            artifact.get("resourceReview", {}).get("approved", False)
        ),
        "binary": binary_evidence,
        "elf": elf_evidence,
        "linkerMap": map_evidence,
        "toolchain": artifact.get("toolchain"),
        "flash": flash,
        "linkedProgram": {
            "bytes": log_program_bytes,
            "imagePackagingBytes": image_packaging_bytes,
        },
        "internalMemory": {
            "globalBytes": internal_globals,
            "remainingBytes": internal_remaining,
            "maximumBytes": internal_maximum,
            "usagePercentReported": float(memory_match.group(2)),
        },
        "baseline": {
            "commit": args.baseline_commit,
            "programBytes": args.baseline_program_bytes,
            "internalGlobalBytes": args.baseline_internal_globals,
        },
        "delta": {
            "programBytes": int(binary_evidence["bytes"]) - args.baseline_program_bytes,
            "internalGlobalBytes": internal_globals - args.baseline_internal_globals,
        },
        "largestSymbols": review.get("largestSymbols"),
        "duplicateImplementationReview": duplicate,
        "buildLog": evidence(args.build_log),
        "artifactManifest": evidence(args.artifact),
        "resourceReview": evidence(args.resource_review),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(
        json.dumps(
            {
                "output": str(args.output),
                "programBytes": binary_evidence["bytes"],
                "programDeltaBytes": summary["delta"]["programBytes"],
                "internalGlobalBytes": internal_globals,
                "internalGlobalDeltaBytes": summary["delta"]["internalGlobalBytes"],
                "tier": flash.get("tier"),
            }
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
