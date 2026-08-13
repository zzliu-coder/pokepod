#!/usr/bin/env python3
"""Behavior and build-script contracts for the fast/release build split."""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "build-input-fingerprint.py"


def fingerprint(*args: str) -> str:
    return subprocess.check_output(
        ["python3", str(TOOL), *args], text=True
    ).strip()


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    source = root / "src"
    source.mkdir()
    (source / "a.cpp").write_text("int a = 1;\n")
    (source / "ignored.bin").write_bytes(b"ignored")
    config = root / "sdkconfig"
    config.write_text("CONFIG_TEST=y\n")

    first = fingerprint("--tree", str(source), "--file", str(config),
                        "--literal", "fqbn=test")
    second = fingerprint("--literal", "fqbn=test", "--file", str(config),
                         "--tree", str(source))
    assert first == second

    (source / "ignored.bin").write_bytes(b"still ignored")
    assert first == fingerprint("--tree", str(source), "--file", str(config),
                                "--literal", "fqbn=test")

    (source / "a.cpp").write_text("int a = 2;\n")
    assert first != fingerprint("--tree", str(source), "--file", str(config),
                                "--literal", "fqbn=test")

build = (ROOT / "firmware" / "build.sh").read_text()
entry = (ROOT / "firmware" / "PokePodAmoled" / "PokePodAmoled.ino").read_text()
app = (ROOT / "firmware" / "PokePodAmoled" / "PokePodApp.cpp").read_text()
verify = (ROOT / "verify.sh").read_text()
gfx_header = (ROOT / "firmware" / "PokePodAmoled" / "PokePodGfx.h").read_text()
gfx_manifest_path = ROOT / "firmware" / "gfx-minimal-files.txt"
gfx_manifest = [
    line for line in gfx_manifest_path.read_text().splitlines()
    if line and not line.startswith("#")
]

assert 'BUILD_MODE=${POKEPOD_BUILD_MODE:-fast}' in build
assert 'BUILD_DIR="$WORK_DIR/build-$BUILD_MODE"' in build
assert 'CLEAN_FLAG=--clean' in build
assert 'unset SOURCE_DATE_EPOCH' in build
assert 'export SOURCE_DATE_EPOCH' in build
assert '--literal "source-date-epoch=$SOURCE_DATE_EPOCH_VALUE"' in build
assert 'CACHE HIT' in build
assert 'build-input-fingerprint.py' in build
assert 'GFX_MINIMAL_LIBRARY="$WORK_DIR/gfx-minimal/$GFX_VIEW_ID"' in build
assert '--tree "$GFX_MINIMAL_LIBRARY/src"' in build
assert '--library "$GFX_MINIMAL_LIBRARY"' in build
assert '--jobs 0' in build
assert '"$SCRIPT_DIR/firmware/build.sh" --release' in verify
assert len(entry.splitlines()) <= 8
assert "void setup()" in app and "void loop()" in app
assert "Arduino_GFX_Library.h" not in gfx_header
assert len(gfx_manifest) == len(set(gfx_manifest))
assert len(gfx_manifest) < 30
assert {
    "Arduino_GFX.cpp",
    "Arduino_TFT.cpp",
    "Arduino_OLED.cpp",
    "canvas/Arduino_Canvas_Indexed.cpp",
    "databus/Arduino_ESP32QSPI.cpp",
    "display/Arduino_SH8601.cpp",
    "display/Arduino_CO5300.cpp",
}.issubset(gfx_manifest)

print("PASS test_build_input_fingerprint")
