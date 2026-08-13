#!/usr/bin/env python3
"""Prove the locked ESP32 compiler honors SOURCE_DATE_EPOCH for date macros."""

from __future__ import annotations

from datetime import datetime, timezone
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from pokepod_build_env import BuildEnvironmentError, resolve_build_environment  # noqa: E402


try:
    environment = resolve_build_environment()
except BuildEnvironmentError as exc:
    raise SystemExit(f"toolchain gate unavailable: {exc}") from exc

tools_root = Path(environment.arduino_data_dir) / "packages/esp32/tools/esp-x32"
compilers = sorted(tools_root.glob("*/bin/xtensa-esp32s3-elf-g++"))
assert compilers, f"ESP32-S3 compiler missing below: {tools_root}"
compiler = compilers[-1]

epoch = 1_700_000_000
expected = datetime.fromtimestamp(epoch, timezone.utc)
expected_date = expected.strftime("%b %d %Y").replace(" 0", "  ")
expected_value = f'"{expected_date}" "{expected:%H:%M:%S}"'

with tempfile.TemporaryDirectory(prefix="pokepod-source-date-epoch-") as raw:
    fixture = Path(raw) / "date-macros.cpp"
    fixture.write_text(
        "POKEPOD_BUILD_EPOCH_UTC __DATE__ __TIME__\n", encoding="utf-8"
    )

    outputs: list[str] = []
    for ambient_tz in ("UTC0", "JST-9"):
        process_environment = os.environ.copy()
        process_environment["SOURCE_DATE_EPOCH"] = str(epoch)
        process_environment["TZ"] = ambient_tz
        output = subprocess.check_output(
            [
                str(compiler), "-E", "-P", "-x", "c++",
                f"-DPOKEPOD_BUILD_EPOCH_UTC={epoch}", str(fixture),
            ],
            env=process_environment,
            text=True,
        ).strip()
        outputs.append(output)

expected_output = f"{epoch} {expected_value}"
assert outputs == [expected_output, expected_output], (outputs, expected_output)
print(
    "PASS source_date_epoch_toolchain "
    f"(compiler={compiler.name}, value={expected_value})"
)
