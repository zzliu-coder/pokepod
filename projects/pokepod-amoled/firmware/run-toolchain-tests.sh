#!/bin/sh
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
python3 "$PROJECT_DIR/tools/run-python-test-gate.py" toolchain
printf 'PASS toolchain_gate\n'
