#!/usr/bin/env python3
"""Release builds bind compiler date macros to the exact source commit."""

from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
BUILD = (ROOT / "firmware/build.sh").read_text(encoding="utf-8")

revision = subprocess.check_output(
    ["git", "-C", str(ROOT), "rev-parse", "--verify", "HEAD"], text=True
).strip()
commit_epoch = subprocess.check_output(
    ["git", "-C", str(ROOT), "show", "-s", "--format=%ct", revision], text=True
).strip()
assert re.fullmatch(r"[0-9]+", commit_epoch) and int(commit_epoch) > 0

assert "unset SOURCE_DATE_EPOCH" in BUILD
assert 'git -C "$PROJECT_DIR" show -s --format=%ct "$SOURCE_REVISION"' in BUILD
assert "Release build requires a positive Git commit timestamp" in BUILD
assert "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH_VALUE" in BUILD
assert "export SOURCE_DATE_EPOCH" in BUILD
assert '--literal "source-date-epoch=$SOURCE_DATE_EPOCH_VALUE"' in BUILD

epoch_lookup = BUILD.index('git -C "$PROJECT_DIR" show -s --format=%ct')
epoch_export = BUILD.index("export SOURCE_DATE_EPOCH", epoch_lookup)
compile_call = BUILD.index('"$ARDUINO_CLI" compile', epoch_export)
assert epoch_lookup < epoch_export < compile_call

print(
    "PASS release_reproducibility_contract "
    f"(source={revision[:12]}, source_date_epoch={commit_epoch})"
)
