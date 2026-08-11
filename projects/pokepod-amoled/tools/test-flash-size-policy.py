#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile


SOURCE = Path(__file__).with_name("flash-size-policy.py")
SPEC = importlib.util.spec_from_file_location("pokepod_flash_size_policy", SOURCE)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

slot = 0x300000
assert MODULE.evaluate(2_350_895, slot)["tier"] == "green"
assert MODULE.evaluate(2_359_295, slot)["tier"] == "green"
assert MODULE.evaluate(2_359_296, slot)["tier"] == "yellow"
assert MODULE.evaluate(2_516_582, slot)["tier"] == "yellow"
assert MODULE.evaluate(2_516_583, slot)["tier"] == "orange"
assert MODULE.evaluate(2_673_868, slot)["tier"] == "orange"
assert MODULE.evaluate(2_673_869, slot)["tier"] == "red"

with tempfile.TemporaryDirectory(prefix="pokepod-flash-policy-") as raw:
    report = Path(raw) / "report.json"
    allowed = subprocess.run(
        [
            sys.executable,
            str(SOURCE),
            "--program-bytes",
            "2404899",
            "--output",
            str(report),
            "--enforce",
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    assert allowed.returncode == 0
    assert '"tier": "yellow"' in report.read_text(encoding="utf-8")

    binary = Path(raw) / "firmware.bin"
    binary.write_bytes(b"x" * 2_404_899)
    review = Path(raw) / "resource-review.json"
    review.write_text(
        json.dumps(
            {
                "schema": "pokepod.resource-review.v1",
                "sourceRevision": "abc",
                "binarySha256": __import__("hashlib").sha256(binary.read_bytes()).hexdigest(),
                "programBytes": len(binary.read_bytes()),
                "tier": "yellow",
                "baseline": {
                    "commit": "base",
                    "programBytes": 2_338_335,
                    "deltaBytes": len(binary.read_bytes()) - 2_338_335,
                },
                "largestSymbols": [{"sizeBytes": 100, "type": "T", "name": "main"}],
                "elf": {"file": "firmware.elf", "bytes": 3, "sha256": "1" * 64},
                "linkerMap": {"file": "firmware.map", "bytes": 3, "sha256": "2" * 64},
                "symbolSourceElfSha256": "1" * 64,
                "duplicateImplementationReview": {
                    "status": "pass",
                    "evidence": ["map checked"],
                    "forbiddenSymbolRegexes": ["legacyDuplicate"],
                    "matchedSymbols": [],
                },
                "nonessentialFeaturesFrozen": False,
                "sizeReductionReview": {"status": "not-required", "evidence": []},
            }
        ),
        encoding="utf-8",
    )
    reviewed = subprocess.run(
        [
            sys.executable,
            str(SOURCE),
            "--program-bytes",
            str(len(binary.read_bytes())),
            "--enforce",
            "--require-release-review",
            "--review",
            str(review),
            "--source-revision",
            "abc",
            "--binary",
            str(binary),
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    assert reviewed.returncode == 0

    review_payload = json.loads(review.read_text(encoding="utf-8"))
    review_payload["baseline"]["deltaBytes"] = 0
    review.write_text(json.dumps(review_payload), encoding="utf-8")
    wrong_delta = subprocess.run(reviewed.args, text=True, capture_output=True, check=False)
    assert wrong_delta.returncode == 5
    assert "delta mismatch" in wrong_delta.stderr

    review_payload["baseline"]["deltaBytes"] = len(binary.read_bytes()) - review_payload["baseline"]["programBytes"]
    review_payload["binarySha256"] = "0" * 64
    review.write_text(json.dumps(review_payload), encoding="utf-8")
    mismatched = subprocess.run(reviewed.args, text=True, capture_output=True, check=False)
    assert mismatched.returncode == 5
    assert "digest mismatch" in mismatched.stderr

    blocked = subprocess.run(
        [
            sys.executable,
            str(SOURCE),
            "--program-bytes",
            "2673869",
            "--enforce",
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    assert blocked.returncode == 4

print("PASS test-flash-size-policy")
