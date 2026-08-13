#!/usr/bin/env python3
"""Keep the repository CI lane aligned with the audit candidate contract."""

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
    "candidate-manifest.json",
    "verify-fast-candidate-evidence.py",
    "--source-revision \"$GITHUB_SHA\"",
    "if-no-files-found: error",
    "\"$BIN\" \"$ELF\" \"$MAP\" \"$BUILD_LOG\"",
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
):
    assert forbidden not in source, f"audit workflow performs forbidden action: {forbidden}"
assert "permissions:\n  contents: read" in source
assert "sourceDirty" not in source  # clean status is validated by the evidence writer.
uses = re.findall(r"^\s*uses:\s*([^#\s]+)(?:\s*#\s*(.+))?$", source, re.MULTILINE)
assert uses, "workflow must use pinned third-party actions"
for reference, comment in uses:
    assert re.fullmatch(r"[^/@]+/[^/@]+@[0-9a-f]{40}", reference), reference
    assert comment and re.search(r"v\d", comment), f"action version comment missing: {reference}"

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
