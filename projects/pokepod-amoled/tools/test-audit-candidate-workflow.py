#!/usr/bin/env python3
"""Keep the repository CI lane aligned with the audit candidate contract."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
workflow = ROOT / ".github/workflows/pokepod-audit-candidate.yml"
assert workflow.is_file(), f"audit workflow missing: {workflow}"
source = workflow.read_text(encoding="utf-8")
for required in (
    "./firmware/run-source-only-tests.sh",
    "./firmware/run-asset-tests.sh",
    "python3 tools/run-cpp-host-tests.py",
    "python3 tools/run-python-test-gate.py repository",
    "version: 1.5.0",
    "esp32:esp32@3.3.8",
    "./firmware/run-toolchain-tests.sh",
    "./firmware/build.sh --fast --force",
    "write-resource-review.py",
    "write-fast-candidate-summary.py",
    "PokePodAmoled.ino.elf",
    "PokePodAmoled.ino.map",
    "2278bb11af4c97f83e71e6e3529b440e80810419",
    "2401888",
    "168060",
    "actions/upload-artifact@v4",
):
    assert required in source, f"audit workflow contract missing: {required}"
for forbidden in (
    "./firmware/build.sh --release",
    "./flash.sh",
    "esptool",
    "serial",
    "merge_pull_request",
):
    assert forbidden not in source, f"audit workflow performs forbidden action: {forbidden}"
assert "permissions:\n  contents: read" in source
assert "sourceDirty" not in source  # clean status is validated by the evidence writer.
print("PASS test-audit-candidate-workflow")
