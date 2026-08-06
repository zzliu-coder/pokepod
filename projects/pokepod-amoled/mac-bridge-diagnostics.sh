#!/bin/sh
set -eu

# Compatibility entry point retained for older local commands.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$SCRIPT_DIR/mac-dictation-diagnostics.sh" "$@"
