#!/usr/bin/env python3
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


SCRIPT = Path(__file__).with_name("write-resource-review.py")

with tempfile.TemporaryDirectory(prefix="pokepod-resource-review-") as raw:
    root = Path(raw)
    binary = root / "firmware.bin"
    binary.write_bytes(b"firmware")
    elf = root / "firmware.elf"
    elf.write_bytes(b"elf")
    linker_map = root / "firmware.map"
    linker_map.write_bytes(b"map")
    nm = root / "fake-nm"
    nm.write_text(
        "#!/bin/sh\n"
        "printf '%s\\n' '42000000 00000020 T small' '42000020 00000100 T large'\n",
        encoding="utf-8",
    )
    os.chmod(nm, 0o755)
    output = root / "review.json"
    subprocess.check_call(
        [
            sys.executable,
            str(SCRIPT),
            "--output",
            str(output),
            "--source-revision",
            "abc",
            "--binary",
            str(binary),
            "--tier",
            "yellow",
            "--baseline-commit",
            "base",
            "--baseline-bytes",
            "4",
            "--nm",
            str(nm),
            "--elf",
            str(elf),
            "--map",
            str(linker_map),
            "--duplicate-evidence",
            "legacy Link symbols absent",
            "--forbidden-symbol-regex",
            "legacyDuplicate",
        ]
    )
    report = json.loads(output.read_text(encoding="utf-8"))
    assert report["programBytes"] == len(b"firmware")
    assert report["baseline"]["deltaBytes"] == len(b"firmware") - 4
    assert report["largestSymbols"][0]["name"] == "large"
    assert report["elf"]["sha256"] == report["symbolSourceElfSha256"]
    assert report["linkerMap"]["bytes"] == 3
    assert report["duplicateImplementationReview"]["status"] == "pass"
    assert report["duplicateImplementationReview"]["matchedSymbols"] == []


    # Green candidates still need reproducible symbol and delta evidence in CI,
    # even though a release review is not mandatory below the yellow threshold.
    green_output = root / "green-review.json"
    subprocess.check_call(
        [
            sys.executable,
            str(SCRIPT),
            "--output",
            str(green_output),
            "--source-revision",
            "green",
            "--binary",
            str(binary),
            "--tier",
            "green",
            "--baseline-commit",
            "base",
            "--baseline-bytes",
            "4",
            "--nm",
            str(nm),
            "--elf",
            str(elf),
            "--map",
            str(linker_map),
            "--duplicate-evidence",
            "legacy Link symbols absent",
            "--forbidden-symbol-regex",
            "legacyDuplicate",
        ]
    )
    green = json.loads(green_output.read_text(encoding="utf-8"))
    assert green["tier"] == "green"
    assert green["duplicateImplementationReview"]["status"] == "pass"

print("PASS test-resource-review")
