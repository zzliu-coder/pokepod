#!/bin/sh
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

"$SCRIPT_DIR/run-source-only-tests.sh"
"$SCRIPT_DIR/run-asset-tests.sh"
"$SCRIPT_DIR/run-toolchain-tests.sh"
python3 "$PROJECT_DIR/tools/run-python-test-gate.py" repository
printf 'PASS complete_host_gate\n'
