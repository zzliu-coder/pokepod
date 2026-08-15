#!/usr/bin/env python3
"""Keep the repository CI lane aligned with the audit candidate contract."""

from collections import Counter
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[3]
workflow = ROOT / ".github/workflows/pokepod-audit-candidate.yml"
assert workflow.is_file(), f"audit workflow missing: {workflow}"
source = workflow.read_text(encoding="utf-8")
for required in (
    "./tools/bootstrap-ci.sh --install",
    "./firmware/run-source-only-tests.sh",
    "./firmware/run-asset-tests.sh",
    "python3 tools/run-cpp-host-tests.py",
    "python3 tools/run-python-test-gate.py repository",
    "ARDUINO_CLI_VERSION: 1.5.0",
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
    "POKEPOD_CPP_COMPILE_TIMEOUT_SECONDS",
    "POKEPOD_CPP_RUN_TIMEOUT_SECONDS",
    "POKEPOD_PYTHON_TEST_TIMEOUT_SECONDS",
    "install-arduino-cli.sh",
    "$BUILD/sketch/BleVoiceService.cpp.o",
    "$BUILD/sketch/CapsuleLibrary.cpp.o",
    "file \"$ELF\" | grep -q 'ELF '",
    "stage-fast-candidate-evidence.py",
    "verify-fast-candidate-evidence.py",
    "--candidate-dir \"$CANDIDATE_DIR\"",
    "CANDIDATE_SHA: ${{ github.event.pull_request.head.sha || github.sha }}",
    "ref: ${{ env.CANDIDATE_SHA }}",
    "--source-revision \"$CANDIDATE_SHA\"",
    "name: pokepod-fast-candidate-${{ env.CANDIDATE_SHA }}",
    "if-no-files-found: error",
    "--binary \"$BIN\" --elf \"$ELF\" --map \"$MAP\"",
):
    assert required in source, f"audit workflow contract missing: {required}"
for forbidden in (
    "./firmware/build.sh --release",
    "./flash.sh",
    "esptool",
    "serial",
    "merge_pull_request",
    "if: always()",
    "arduino/setup-arduino-cli@",
    "--source-revision \"$GITHUB_SHA\"",
    "name: pokepod-fast-candidate-${{ github.sha }}",
):
    assert forbidden not in source, f"audit workflow performs forbidden action: {forbidden}"
assert "permissions:\n  contents: read" in source
assert source.count("ref: ${{ env.CANDIDATE_SHA }}") == 2
assert "sourceDirty" not in source  # clean status is validated by staged evidence.
uses = re.findall(r"^\s*uses:\s*([^#\s]+)(?:\s*#\s*(\S+))?\s*$", source, re.MULTILINE)
expected_uses = Counter(
    {
        ("actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1", "v7.0.1"): 2,
        ("actions/cache@caa296126883cff596d87d8935842f9db880ef25", "v5.1.0"): 1,
        ("actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a", "v7.0.1"): 1,
    }
)
assert Counter(uses) == expected_uses, (
    "third-party action pin/version allowlist mismatch: "
    f"observed={Counter(uses)} expected={expected_uses}"
)

upload_block = source[source.index("- name: Upload Fast candidate evidence") :]
assert (
    "path: projects/pokepod-amoled/work/pokepod-build/github-fast-candidate/"
    in upload_block
)
for forbidden_upload_path in (
    "output/fast/",
    "build-fast/PokePodAmoled.ino.elf",
    "build-fast/PokePodAmoled.ino.map",
    "build-fast.log",
    "resource-review.json",
    "fast-candidate-summary.json",
):
    assert forbidden_upload_path not in upload_block, (
        f"upload action escapes closed candidate directory: {forbidden_upload_path}"
    )

bootstrap_position = source.index("./tools/bootstrap-ci.sh --install")
first_gate_position = min(
    source.index("./firmware/run-source-only-tests.sh"),
    source.index("./firmware/run-toolchain-tests.sh"),
)
assert bootstrap_position < first_gate_position

contract = (ROOT / "projects/pokepod-amoled/tools/ci-repository-contract.md").read_text(
    encoding="utf-8"
)
for requirement in (
    "main` branch protection",
    "Source, asset, sanitizer, audit package",
    "Locked toolchain and clean forced Fast build",
    "源码合同检查只能报告这些要求存在",
):
    assert requirement in contract, f"repository governance contract missing: {requirement}"
print("PASS test-audit-candidate-workflow")
