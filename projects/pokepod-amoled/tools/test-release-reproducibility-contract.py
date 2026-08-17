#!/usr/bin/env python3
"""Release builds bind compiler date macros to the exact source commit."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BUILD = (ROOT / "firmware/build.sh").read_text(encoding="utf-8")
BOARD = (ROOT / "firmware/PokePodAmoled/BoardServices.cpp").read_text(
    encoding="utf-8"
)

assert "unset SOURCE_DATE_EPOCH" in BUILD
assert 'git -C "$PROJECT_DIR" show -s --format=%ct "$SOURCE_REVISION"' in BUILD
assert "Release build requires a positive Git commit timestamp" in BUILD
assert "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH_VALUE" in BUILD
assert "export SOURCE_DATE_EPOCH" in BUILD
assert "BUILD_EPOCH_CPP_FLAG=-DPOKEPOD_BUILD_EPOCH_UTC=$SOURCE_DATE_EPOCH_VALUE" in BUILD
assert '--build-property "compiler.cpp.extra_flags=$BUILD_EPOCH_CPP_FLAG"' in BUILD
assert 'POKEPOD_SOURCE_REVISION=\\"$SOURCE_REVISION\\"' in BUILD
assert 'POKEPOD_SOURCE_TREE=\\"$SOURCE_TREE\\"' in BUILD
assert 'POKEPOD_APP_ELF_SHA256=\\"unknown\\"' in BUILD
assert 'POKEPOD_SOURCE_REVISION=\\\\\\"$SOURCE_REVISION' not in BUILD
assert '--literal "source-date-epoch=$SOURCE_DATE_EPOCH_VALUE"' in BUILD
assert 'if [ "$BUILD_MODE" = release ]; then\n  for build_argument in "$@"' in BUILD
assert "--build-property|--build-property=*)" in BUILD
assert "Release build rejects caller-supplied --build-property" in BUILD
assert "#if defined(POKEPOD_BUILD_EPOCH_UTC)" in BOARD
assert "setUtcEpoch(static_cast<time_t>(POKEPOD_BUILD_EPOCH_UTC))" in BOARD
assert "firmware_build_epoch_utc" in BOARD
assert "#else" in BOARD
assert "buildLocalDateTimeToUtcEpoch(__DATE__, __TIME__" in BOARD
assert "firmware_build_time_local" in BOARD

epoch_lookup = BUILD.index('git -C "$PROJECT_DIR" show -s --format=%ct')
epoch_export = BUILD.index("export SOURCE_DATE_EPOCH", epoch_lookup)
compile_call = BUILD.index('"$ARDUINO_CLI" compile', epoch_export)
assert epoch_lookup < epoch_export < compile_call

argument_guard = BUILD.index('for build_argument in "$@"')
commit_guard = BUILD.index('if [ -z "$SOURCE_REVISION"')
assert argument_guard < commit_guard

for inherited_epoch, arguments in (
    ("", ("--build-property", "compiler.cpp.extra_flags=-DPOKEPOD_BUILD_EPOCH_UTC=0")),
    ("9999999999", ("--build-property=compiler.cpp.extra_flags=-DPOKEPOD_BUILD_EPOCH_UTC=1",)),
):
    environment = os.environ.copy()
    environment["SOURCE_DATE_EPOCH"] = inherited_epoch
    rejected = subprocess.run(
        ["sh", str(ROOT / "firmware/build.sh"), "--release", "--", *arguments],
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )
    assert rejected.returncode == 64, rejected
    assert "Release build rejects caller-supplied --build-property" in rejected.stderr

# A packaged source tree can have no Git metadata. The argument contract must
# still win before the later release identity checks return exit 65.
with tempfile.TemporaryDirectory(prefix="pokepod-release-no-git-") as raw:
    package_root = Path(raw)
    packaged_build = package_root / "firmware/build.sh"
    packaged_build.parent.mkdir(parents=True)
    shutil.copy2(ROOT / "firmware/build.sh", packaged_build)
    for argument in (
        "--build-property",
        "--build-property=compiler.cpp.extra_flags=-DPOKEPOD_BUILD_EPOCH_UTC=1",
    ):
        rejected = subprocess.run(
            ["sh", str(packaged_build), "--release", "--", argument],
            text=True,
            capture_output=True,
            check=False,
        )
        assert rejected.returncode == 64, rejected
        assert "Release build rejects caller-supplied --build-property" in rejected.stderr

git_root = subprocess.run(
    ["git", "-C", str(ROOT), "rev-parse", "--show-toplevel"],
    text=True,
    capture_output=True,
    check=False,
)
if git_root.returncode == 0:
    revision = subprocess.check_output(
        ["git", "-C", str(ROOT), "rev-parse", "--verify", "HEAD"], text=True
    ).strip()
    commit_epoch = subprocess.check_output(
        ["git", "-C", str(ROOT), "show", "-s", "--format=%ct", revision], text=True
    ).strip()
    assert re.fullmatch(r"[0-9]+", commit_epoch) and int(commit_epoch) > 0
    detail = f"source={revision[:12]}, source_date_epoch={commit_epoch}"
else:
    detail = "source-only-no-git"

print(f"PASS release_reproducibility_contract ({detail})")
