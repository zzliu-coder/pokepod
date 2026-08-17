#!/usr/bin/env python3
"""Verify one exact-SHA, closed Fast candidate directory before upload."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


PAYLOAD_NAMES = (
    "PokePodAmoled.ino.bin",
    "PokePodAmoled.ino.elf",
    "PokePodAmoled.ino.map",
    "build-fast.log",
    "artifact.json",
    "flash-resource.json",
    "resource-review.json",
    "fast-candidate-summary.json",
)
MANIFEST_NAME = "candidate-manifest.json"
CHECKSUM_NAME = "candidate-sha256.txt"
EXPECTED_NAMES = frozenset((*PAYLOAD_NAMES, MANIFEST_NAME, CHECKSUM_NAME))
PROGRAM_RE = re.compile(r"Sketch uses\s+([0-9,]+)\s+bytes", re.IGNORECASE)
MEMORY_RE = re.compile(
    r"Global variables use\s+([0-9,]+)\s+bytes.*?leaving\s+([0-9,]+)\s+bytes.*?Maximum is\s+([0-9,]+)\s+bytes",
    re.IGNORECASE,
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def require_evidence(document: dict[str, object], key: str, path: Path,
                     label: str) -> None:
    value = document.get(key)
    require(isinstance(value, dict), f"{label} evidence missing")
    require(value.get("file") == path.name and
            value.get("bytes") == path.stat().st_size and
            value.get("sha256") == sha256(path),
            f"{label} evidence differs from payload")


def validate_payload_semantics(paths: dict[str, Path], revision: str) -> None:
    """Check that every evidence document describes the same binary/policy."""
    artifact = json.loads(paths["artifact.json"].read_text(encoding="utf-8"))
    flash = json.loads(paths["flash-resource.json"].read_text(encoding="utf-8"))
    review = json.loads(paths["resource-review.json"].read_text(encoding="utf-8"))
    summary = json.loads(paths["fast-candidate-summary.json"].read_text(encoding="utf-8"))
    binary_path = paths["PokePodAmoled.ino.bin"]
    binary_bytes = binary_path.stat().st_size
    binary_sha = sha256(binary_path)
    artifact_binary = artifact.get("binary")
    require(isinstance(artifact_binary, dict), "artifact binary object missing")
    require(artifact.get("sourceRevision") == revision, "artifact SHA mismatch")
    require(artifact.get("sourceDirty") is False, "artifact source is dirty")
    require(artifact.get("resourcePolicy") == flash,
            "artifact and flash policy differ")
    require(artifact_binary.get("sizeBytes") == binary_bytes,
            "artifact binary size differs from payload")
    require(artifact_binary.get("sha256") == binary_sha,
            "artifact binary SHA differs from payload")
    require(artifact_binary.get("usagePercent") == flash.get("percent") and
            artifact_binary.get("resourceTier") == flash.get("tier") and
            artifact_binary.get("remainingBytes") == flash.get("remainingBytes"),
            "artifact binary resource fields differ from flash policy")
    require(flash.get("programBytes") == binary_bytes,
            "flash policy program size differs from payload")
    require(review.get("sourceRevision") == revision,
            "resource review SHA mismatch")
    require(review.get("binarySha256") == binary_sha,
            "resource review binary SHA differs from payload")
    require(review.get("programBytes") == binary_bytes,
            "resource review program size differs from payload")
    require(review.get("tier") == flash.get("tier"),
            "resource review tier differs from flash policy")
    require_evidence(review, "elf", paths["PokePodAmoled.ino.elf"],
                     "resource review ELF")
    require_evidence(review, "linkerMap", paths["PokePodAmoled.ino.map"],
                     "resource review linker map")
    require(review.get("symbolSourceElfSha256") ==
            review["elf"].get("sha256"),
            "resource review symbol ELF differs from ELF evidence")
    duplicate = review.get("duplicateImplementationReview")
    require(isinstance(duplicate, dict) and duplicate.get("status") == "pass",
            "duplicate implementation review is not pass")
    require(duplicate.get("matchedSymbols") == [],
            "duplicate implementation review contains matches")
    baseline = review.get("baseline")
    require(isinstance(baseline, dict), "resource review baseline missing")
    require(isinstance(baseline.get("programBytes"), int) and
            isinstance(baseline.get("deltaBytes"), int),
            "resource review baseline numbers missing")
    require(baseline.get("deltaBytes") ==
            review.get("programBytes") - baseline.get("programBytes"),
            "resource review baseline delta mismatch")
    summary_binary = summary.get("binary")
    require(isinstance(summary_binary, dict), "summary binary object missing")
    require(summary_binary.get("bytes") == binary_bytes and
            summary_binary.get("sha256") == binary_sha,
            "summary binary differs from payload")
    require(summary.get("flash") == flash,
            "summary and flash policy differ")
    require(summary.get("sourceRevision") == revision and
            summary.get("sourceClean") is True,
            "summary source identity mismatch")
    summary_delta = summary.get("delta")
    require(isinstance(summary_delta, dict) and
            summary_delta.get("programBytes") ==
            binary_bytes - baseline.get("programBytes"),
            "summary binary delta mismatch")
    require_evidence(summary, "elf", paths["PokePodAmoled.ino.elf"],
                     "summary ELF")
    require_evidence(summary, "linkerMap", paths["PokePodAmoled.ino.map"],
                     "summary linker map")
    require_evidence(summary, "buildLog", paths["build-fast.log"],
                     "summary build log")
    summary_baseline = summary.get("baseline")
    require(isinstance(summary_baseline, dict), "summary baseline missing")
    require(isinstance(baseline.get("commit"), str) and
            isinstance(baseline.get("programBytes"), int) and
            isinstance(summary_baseline.get("programBytes"), int) and
            isinstance(summary_baseline.get("internalGlobalBytes"), int),
            "summary/review baseline types missing")
    require(summary_baseline.get("commit") == baseline.get("commit") and
            summary_baseline.get("programBytes") == baseline.get("programBytes") and
            summary_baseline.get("deltaBytes") ==
            binary_bytes - baseline.get("programBytes") and
            summary_baseline.get("internalGlobalBytes") is not None,
            "summary baseline differs from resource review")
    require(summary_delta.get("programBytes") ==
            review.get("programBytes") - baseline.get("programBytes") and
            summary_delta.get("programBytes") ==
            binary_bytes - summary_baseline.get("programBytes"),
            "summary/review program delta differs")
    linked = summary.get("linkedProgram")
    require(isinstance(linked, dict), "summary linked program missing")
    require(isinstance(linked.get("bytes"), int) and
            isinstance(linked.get("imagePackagingBytes"), int) and
            linked.get("bytes") + linked.get("imagePackagingBytes") == binary_bytes and
            0 <= linked.get("imagePackagingBytes") <= 4096,
            "summary linked program packaging differs")
    memory = summary.get("internalMemory")
    require(isinstance(memory, dict), "summary internal memory missing")
    require(memory.get("globalBytes") + memory.get("remainingBytes") ==
            memory.get("maximumBytes"),
            "summary internal memory arithmetic differs")
    build_log = paths["build-fast.log"].read_text(encoding="utf-8", errors="replace")
    program_match = PROGRAM_RE.search(build_log)
    memory_match = MEMORY_RE.search(build_log)
    require(program_match is not None and memory_match is not None,
            "build log size evidence missing")
    linked_bytes = int(program_match.group(1).replace(",", ""))
    global_bytes = int(memory_match.group(1).replace(",", ""))
    remaining_bytes = int(memory_match.group(2).replace(",", ""))
    maximum_bytes = int(memory_match.group(3).replace(",", ""))
    require(linked.get("bytes") == linked_bytes and
            memory.get("globalBytes") == global_bytes and
            memory.get("remainingBytes") == remaining_bytes and
            memory.get("maximumBytes") == maximum_bytes,
            "summary size facts differ from build log")
    require(summary_delta.get("internalGlobalBytes") ==
            memory.get("globalBytes") - summary_baseline.get("internalGlobalBytes"),
            "summary internal RAM delta differs from baseline")
    artifact_review = artifact.get("resourceReview")
    require(isinstance(artifact_review, dict), "artifact resource review missing")
    approved = artifact_review.get("approved")
    require(isinstance(approved, bool), "artifact resource review approval missing")
    require(summary.get("resourceReviewApproved") is approved,
            "summary resource review approval differs from artifact")
    for field, filename in (("artifactManifest", "artifact.json"),
                            ("resourceReview", "resource-review.json")):
        evidence = summary.get(field)
        require(isinstance(evidence, dict), f"summary {field} missing")
        target = paths[filename]
        require(evidence.get("file") == filename and
                evidence.get("bytes") == target.stat().st_size and
                evidence.get("sha256") == sha256(target),
                f"summary {field} evidence mismatch")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--candidate-dir", type=Path, required=True)
    args = parser.parse_args()

    revision = args.source_revision.lower()
    require(len(revision) == 40 and all(c in "0123456789abcdef" for c in revision),
            "source revision must be one full Git SHA-1")
    candidate_dir = args.candidate_dir
    require(candidate_dir.is_dir() and not candidate_dir.is_symlink(),
            f"candidate directory missing or invalid: {candidate_dir}")
    entries = tuple(candidate_dir.iterdir())
    observed_names = {path.name for path in entries}
    require(observed_names == EXPECTED_NAMES,
            "candidate directory file set mismatch; "
            f"unexpected={sorted(observed_names - EXPECTED_NAMES)}, "
            f"missing={sorted(EXPECTED_NAMES - observed_names)}")
    paths = {name: candidate_dir / name for name in EXPECTED_NAMES}
    for path in paths.values():
        require(path.is_file() and not path.is_symlink() and path.stat().st_size > 0,
                f"candidate evidence missing, empty, or non-regular: {path}")

    candidate = json.loads(paths[MANIFEST_NAME].read_text(encoding="utf-8"))
    validate_payload_semantics(paths, revision)
    artifact = json.loads(paths["artifact.json"].read_text(encoding="utf-8"))
    flash = json.loads(paths["flash-resource.json"].read_text(encoding="utf-8"))
    review = json.loads(paths["resource-review.json"].read_text(encoding="utf-8"))
    summary = json.loads(paths["fast-candidate-summary.json"].read_text(encoding="utf-8"))
    require(flash.get("schema") == "pokepod.flash-size-policy.v1",
            "flash resource schema mismatch")
    require(flash.get("releaseAllowed") is True,
            "flash resource policy blocks this candidate")
    require(candidate.get("sourceRevision") == revision,
            "candidate manifest SHA mismatch")
    require(candidate.get("schema") == "pokepod.github-fast-candidate.v2",
            "candidate manifest schema mismatch")
    require(candidate.get("sourceClean") is True, "candidate manifest source is dirty")
    status = candidate.get("status")
    require(candidate.get("resourceReviewApproved") is
            artifact.get("resourceReview", {}).get("approved"),
            "candidate resource review approval differs from artifact")
    require(status in {"pending", "verified"}, "candidate manifest status is invalid")
    if status == "verified":
        require(candidate.get("verifiedBy") ==
                "verify-fast-candidate-evidence.v1",
                "candidate verification provenance missing")

    expected_digests = {
        name: sha256(paths[name]) for name in (*PAYLOAD_NAMES, MANIFEST_NAME)
    }
    manifest_lines = paths[CHECKSUM_NAME].read_text(encoding="utf-8").splitlines()
    observed: dict[str, str] = {}
    for line in manifest_lines:
        checksum, separator, raw_path = line.partition("  ")
        require(bool(separator), f"invalid SHA-256 manifest line: {line}")
        require(len(checksum) == 64 and all(c in "0123456789abcdef" for c in checksum),
                f"invalid SHA-256 digest: {checksum}")
        require(raw_path == Path(raw_path).name and raw_path not in observed,
                f"invalid or duplicate SHA-256 manifest path: {raw_path}")
        observed[raw_path] = checksum
    require(observed == expected_digests,
            "SHA-256 manifest file set or digest mismatch")

    candidate_files = candidate.get("files")
    require(isinstance(candidate_files, list), "candidate manifest files missing")
    by_name = {entry.get("file"): entry for entry in candidate_files
               if isinstance(entry, dict)}
    require(set(by_name) == set(PAYLOAD_NAMES) and len(candidate_files) == len(PAYLOAD_NAMES),
            "candidate manifest payload file set mismatch")
    for name in PAYLOAD_NAMES:
        path = paths[name]
        entry = by_name.get(name)
        require(isinstance(entry, dict), f"candidate manifest omits {name}")
        require(entry.get("bytes") == path.stat().st_size, f"size mismatch: {name}")
        require(entry.get("sha256") == sha256(path), f"digest mismatch: {name}")

    # Staging deliberately creates an untrusted pending manifest. Only this
    # verifier may promote it after all semantic checks above have passed.
    if status == "pending":
        candidate["status"] = "verified"
        candidate["verifiedBy"] = "verify-fast-candidate-evidence.v1"
        candidate["resourceReviewApproved"] = artifact.get("resourceReview", {}).get(
            "approved"
        )
        paths[MANIFEST_NAME].write_text(
            json.dumps(candidate, indent=2) + "\n", encoding="utf-8"
        )
        paths[CHECKSUM_NAME].write_text(
            "".join(f"{sha256(paths[name])}  {name}\n"
                    for name in (*PAYLOAD_NAMES, MANIFEST_NAME)),
            encoding="utf-8",
        )

    print(f"PASS exact_fast_candidate_evidence ({revision})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
