#!/usr/bin/env python3
"""Keep the ROM rescue shell lane explicit and fail closed."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "flash.sh").read_text(encoding="utf-8")
validator = (ROOT / "tools" / "validate-rescue-partitions.py").read_text(encoding="utf-8")

assert "--rom-rescue" in source
assert "--rescue-target" in source
assert "--known-good-slot" in source
assert "rescue_target_and_known_good_must_differ" in source
assert "validate-rescue-partitions.py" in source
assert "--target-slot app0" in source
assert "--offset 0x8000 --size 0x1000" in source
assert "--offset 0xE000 --size 0x2000" in source
assert "--offset 0x10000 --size 0x300000" in source
assert "--offset 0x310000 --size 0x300000" in source
assert 'targetSlot": "write-candidate-only"' in source
assert 'knownGoodSlot": "never-write"' in source
assert "rescue_known_good_slot_changed" in source
assert "runtime" in source
assert "imageIdentity" in validator and "appElfSha256" in validator
assert "rescue_running_partition" in validator
assert "rescue_source_revision_mismatch" in validator
assert "rescue_firmware_version_mismatch" in validator
assert "rescue_app_elf_sha_mismatch" in validator
assert "image_identity_app_elf_sha" in validator and "_invalid" in validator

# Identity must be checked again after every prewrite backup and before the
# first transfer command in either lane.
prewrite = source.index("PREWRITE_DEVICE_KEY=$(python3")
first_write = source.index('python3 "$TRANSFER_SCRIPT" flash')
assert prewrite < first_write

# Rescue may only pass the target offset to a write operation.  The app1
# offset remains a backup/readback surface unless app1 is selected explicitly.
assert "POST_KNOWN_GOOD_OFFSET=0x310000" in source
assert "--artifact \"$OTADATA_CANDIDATE\"" in source

print("PASS rescue_flash_policy")
