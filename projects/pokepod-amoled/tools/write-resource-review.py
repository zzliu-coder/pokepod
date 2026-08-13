#!/usr/bin/env python3
"""Bind a linker-symbol and duplicate-implementation review to one binary."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


NM_LINE = re.compile(r"^[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+(\S)\s+(.+)$")


def positive_integer(value: str) -> int:
    parsed = int(value, 0)
    if parsed < 0:
        raise argparse.ArgumentTypeError("value must be non-negative")
    return parsed


def read_symbols(nm: Path, elf: Path) -> list[dict[str, object]]:
    result = subprocess.run(
        [str(nm), "-C", "-S", "--size-sort", str(elf)],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "nm failed")
    parsed: list[dict[str, object]] = []
    for line in result.stdout.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        parsed.append(
            {
                "sizeBytes": int(match.group(1), 16),
                "type": match.group(2),
                "name": match.group(3),
            }
        )
    return parsed


def file_evidence(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    return {
        "file": path.name,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--tier", choices=("green", "yellow", "orange"), required=True)
    parser.add_argument("--baseline-commit", required=True)
    parser.add_argument("--baseline-bytes", type=positive_integer, required=True)
    parser.add_argument("--nm", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--map", type=Path, required=True)
    parser.add_argument("--duplicate-evidence", action="append", required=True)
    parser.add_argument("--forbidden-symbol-regex", action="append", required=True)
    parser.add_argument("--nonessential-features-frozen", action="store_true")
    parser.add_argument("--size-reduction-evidence", action="append", default=[])
    args = parser.parse_args()

    binary = args.binary.read_bytes()
    program_bytes = len(binary)
    symbols = read_symbols(args.nm, args.elf)
    if not symbols:
        raise RuntimeError("no linker symbols found")
    forbidden_matches: list[str] = []
    for pattern_source in args.forbidden_symbol_regex:
        pattern = re.compile(pattern_source)
        forbidden_matches.extend(
            str(symbol["name"]) for symbol in symbols if pattern.search(str(symbol["name"]))
        )
    if forbidden_matches:
        raise RuntimeError("forbidden duplicate symbols: " + ", ".join(sorted(set(forbidden_matches))))
    if args.tier == "orange" and (
        not args.nonessential_features_frozen or not args.size_reduction_evidence
    ):
        raise ValueError("orange review requires feature freeze and size reduction evidence")

    report = {
        "schema": "pokepod.resource-review.v1",
        "sourceRevision": args.source_revision,
        "binarySha256": hashlib.sha256(binary).hexdigest(),
        "programBytes": program_bytes,
        "tier": args.tier,
        "baseline": {
            "commit": args.baseline_commit,
            "programBytes": args.baseline_bytes,
            "deltaBytes": program_bytes - args.baseline_bytes,
        },
        "elf": file_evidence(args.elf),
        "linkerMap": file_evidence(args.map),
        "symbolSourceElfSha256": file_evidence(args.elf)["sha256"],
        "largestSymbols": symbols[-40:][::-1],
        "duplicateImplementationReview": {
            "status": "pass",
            "evidence": args.duplicate_evidence,
            "forbiddenSymbolRegexes": args.forbidden_symbol_regex,
            "matchedSymbols": [],
        },
        "nonessentialFeaturesFrozen": args.nonessential_features_frozen,
        "sizeReductionReview": {
            "status": "pass" if args.size_reduction_evidence else "not-required",
            "evidence": args.size_reduction_evidence,
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "programBytes": program_bytes, "tier": args.tier}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
