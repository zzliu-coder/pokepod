#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORKTREE_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
MAC_PROJECT="$WORKTREE_DIR/projects/pokecapsule-mac"

"$SCRIPT_DIR/firmware/run-host-tests.sh"
sh -n "$SCRIPT_DIR/firmware/build.sh" \
  "$SCRIPT_DIR/firmware/run-host-tests.sh" \
  "$SCRIPT_DIR/usb-audio-smoke.sh" \
  "$SCRIPT_DIR/device-acceptance.sh" \
  "$SCRIPT_DIR/end-to-end-acceptance.sh" \
  "$SCRIPT_DIR/flash.sh" \
  "$SCRIPT_DIR/hid-shortcut-smoke.sh" \
  "$SCRIPT_DIR/mac-bridge-diagnostics.sh" \
  "$SCRIPT_DIR/mac-dictation-diagnostics.sh"
/usr/bin/python3 - "$SCRIPT_DIR/cdc-status.py" \
  "$SCRIPT_DIR/sd-capsule-acceptance.py" \
  "$SCRIPT_DIR/provision-pokepod.py" <<'PY'
import pathlib
import sys

for name in sys.argv[1:]:
    source = pathlib.Path(name).read_text(encoding="utf-8")
    compile(source, name, "exec")
PY
zsh -n "$MAC_PROJECT/build-app.sh"
"$SCRIPT_DIR/firmware/build.sh"
swift test --disable-sandbox --package-path "$MAC_PROJECT"
swift build -c release --disable-sandbox --package-path "$MAC_PROJECT" \
  --scratch-path "$MAC_PROJECT/.build"
plutil -lint "$MAC_PROJECT/Resources/Info.plist"
git -C "$WORKTREE_DIR" diff --check
shasum -a 256 "$SCRIPT_DIR/work/pokepod-build/output/PokePodAmoled.ino.bin"
printf 'PASS pokepod_software_gate\n'
