#!/bin/sh
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
FONT_ASSET="$PROJECT_DIR/assets/cjk20.a4"
if [ ! -f "$FONT_ASSET" ]; then
  printf 'asset gate requires the full repository asset: %s\n' "$FONT_ASSET" >&2
  exit 66
fi
python3 "$PROJECT_DIR/tools/run-python-test-gate.py" asset
printf 'PASS asset_gate\n'
