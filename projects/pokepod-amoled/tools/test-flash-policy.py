#!/usr/bin/env python3
"""Keep the ESP32-S3 native-USB flash flow self-starting."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "flash.sh").read_text(encoding="utf-8")

assert 'stty -f "$APP_PORT" 1200' in source
assert '--before no-reset --after watchdog-reset run' in source
assert '--after hard-reset run' not in source

print("PASS flash_policy")
