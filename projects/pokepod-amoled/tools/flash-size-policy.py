#!/usr/bin/env python3
"""Evaluate a PokePod application binary against one OTA slot."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys


DEFAULT_SLOT_BYTES = 0x300000


def parse_integer(value: str) -> int:
    parsed = int(value, 0)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return parsed


def evaluate(program_bytes: int, slot_bytes: int = DEFAULT_SLOT_BYTES) -> dict[str, object]:
    if program_bytes < 0 or slot_bytes <= 0:
        raise ValueError("invalid flash size")
    scaled = program_bytes * 100
    if scaled < slot_bytes * 75:
        tier = "green"
        action = "keep implementation simple"
    elif scaled < slot_bytes * 80:
        tier = "yellow"
        action = "review map, symbol delta and duplicate implementations"
    elif scaled < slot_bytes * 85:
        tier = "orange"
        action = "freeze nonessential features and complete a size reduction review"
    else:
        tier = "red"
        action = "release blocked"
    return {
        "schema": "pokepod.flash-size-policy.v1",
        "programBytes": program_bytes,
        "slotBytes": slot_bytes,
        "remainingBytes": slot_bytes - program_bytes,
        "percent": round(program_bytes * 100.0 / slot_bytes, 4),
        "tier": tier,
        "releaseAllowed": tier != "red",
        "requiredAction": action,
        "thresholds": {
            "greenBelowPercent": 75,
            "yellowBelowPercent": 80,
            "orangeBelowPercent": 85,
            "redAtOrAbovePercent": 85,
        },
    }


def validate_release_review(
    policy: dict[str, object],
    review: dict[str, object],
    source_revision: str,
    binary_sha256: str,
) -> list[str]:
    errors: list[str] = []
    if review.get("schema") != "pokepod.resource-review.v1":
        errors.append("invalid review schema")
    if review.get("sourceRevision") != source_revision:
        errors.append("review source revision mismatch")
    if review.get("binarySha256") != binary_sha256:
        errors.append("review binary digest mismatch")
    if review.get("programBytes") != policy["programBytes"]:
        errors.append("review program size mismatch")
    if review.get("tier") != policy["tier"]:
        errors.append("review resource tier mismatch")
    baseline = review.get("baseline")
    if not isinstance(baseline, dict) or not isinstance(baseline.get("programBytes"), int):
        errors.append("review baseline missing")
    else:
        if not isinstance(baseline.get("commit"), str) or not baseline["commit"].strip():
            errors.append("review baseline commit missing")
        expected_delta = int(policy["programBytes"]) - baseline["programBytes"]
        if baseline.get("deltaBytes") != expected_delta:
            errors.append("review baseline delta mismatch")
    symbols = review.get("largestSymbols")
    if not isinstance(symbols, list) or not symbols:
        errors.append("largest symbol evidence missing")
    for evidence_name in ("elf", "linkerMap"):
        evidence = review.get(evidence_name)
        if (
            not isinstance(evidence, dict)
            or not isinstance(evidence.get("bytes"), int)
            or evidence["bytes"] <= 0
            or not isinstance(evidence.get("sha256"), str)
            or len(evidence["sha256"]) != 64
        ):
            errors.append(f"{evidence_name} evidence missing")
    elf = review.get("elf")
    if isinstance(elf, dict) and review.get("symbolSourceElfSha256") != elf.get("sha256"):
        errors.append("symbol ELF binding mismatch")
    duplicate = review.get("duplicateImplementationReview")
    if not isinstance(duplicate, dict) or duplicate.get("status") != "pass":
        errors.append("duplicate implementation review missing")
    elif not isinstance(duplicate.get("evidence"), list) or not duplicate["evidence"]:
        errors.append("duplicate implementation evidence missing")
    else:
        if not duplicate.get("forbiddenSymbolRegexes"):
            errors.append("duplicate symbol rules missing")
        if duplicate.get("matchedSymbols") != []:
            errors.append("forbidden duplicate symbols present")
    if policy["tier"] == "orange":
        if review.get("nonessentialFeaturesFrozen") is not True:
            errors.append("orange tier feature freeze missing")
        reduction = review.get("sizeReductionReview")
        if not isinstance(reduction, dict) or reduction.get("status") != "pass":
            errors.append("orange tier size reduction review missing")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--program-bytes", type=parse_integer, required=True)
    parser.add_argument("--slot-bytes", type=parse_integer, default=DEFAULT_SLOT_BYTES)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--enforce", action="store_true")
    parser.add_argument("--require-release-review", action="store_true")
    parser.add_argument("--review", type=Path)
    parser.add_argument("--source-revision")
    parser.add_argument("--binary", type=Path)
    args = parser.parse_args()

    report = evaluate(args.program_bytes, args.slot_bytes)
    encoded = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    if args.enforce and not report["releaseAllowed"]:
        return 4
    if args.require_release_review and report["tier"] in ("yellow", "orange"):
        if not args.review or not args.source_revision or not args.binary:
            print("release resource review arguments missing", file=sys.stderr)
            return 5
        if not args.review.is_file() or not args.binary.is_file():
            print("release resource review or binary missing", file=sys.stderr)
            return 5
        review = json.loads(args.review.read_text(encoding="utf-8"))
        binary_sha256 = hashlib.sha256(args.binary.read_bytes()).hexdigest()
        errors = validate_release_review(report, review, args.source_revision, binary_sha256)
        if errors:
            for error in errors:
                print(error, file=sys.stderr)
            return 5
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
