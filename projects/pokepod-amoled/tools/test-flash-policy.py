#!/usr/bin/env python3
"""Keep the ESP32-S3 native-USB flash flow self-starting."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "flash.sh").read_text(encoding="utf-8")
validator = (ROOT / "tools/validate-flash-artifact.py").read_text(encoding="utf-8")

assert 'stty -f "$APP_PORT" 1200' in source
assert '--before usb-reset --after watchdog-reset --no-stub chip-id' in source
assert '--after hard-reset run' not in source
assert 'FLASH_MODE=release' in source
assert 'output/$FLASH_MODE/PokePodAmoled.ino.bin' in source
assert 'validate-flash-artifact.py' in source
assert 'artifact_manifest_binary_sha256' in validator
assert 'artifact_resource_policy_mismatch' in validator
assert 'resource_review_invalid' in validator
assert 'if review_required:' in validator
assert 'release_artifact_source_dirty' in validator
assert 'validate_release_review' in validator
assert 'esp32_region_transfer.py' in source
assert 'hardmac.esp32-region-transfer.v1' in source
assert '--identity-authority' in source
assert 'validate-flash-identity.py' in source
assert 'flash-id' in source
assert '--chunk-size 16384' in source
assert '--before usb-reset' in source
assert '--stub disabled' in source
assert '--max-size 0x300000' in source
assert '--baud 115200' in source
assert 'write_seconds={value[' in source
assert 'readback_seconds=' in source
assert 'run.json' in source
assert 'write-flash 0x10000 "$FIRMWARE_BIN"' not in source
assert '--baud 460800' not in source

print("PASS flash_policy")
