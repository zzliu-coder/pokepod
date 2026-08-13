#!/usr/bin/env python3
"""Keep PokePod's Hard Mac profile and build lanes safely separated."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
profile = json.loads((ROOT / ".hardmac/workflow.json").read_text(encoding="utf-8"))
build = (ROOT / "firmware/build.sh").read_text(encoding="utf-8")
flash = (ROOT / "flash.sh").read_text(encoding="utf-8")
manifest_writer = (ROOT / "tools/write-artifact-manifest.py").read_text(encoding="utf-8")
artifact_validator = (ROOT / "tools/validate-flash-artifact.py").read_text(encoding="utf-8")
identity_validator = (ROOT / "tools/validate-flash-identity.py").read_text(encoding="utf-8")

assert profile["schemaVersion"] == 1
assert profile["kind"] == "hardmac.workflow"
assert profile["maturity"] == "project-verified"
assert profile["safety"] == {
    "storesSecrets": False,
    "storesInstanceIdentifiers": False,
}
fast = profile["lanes"]["fast"]
release = profile["lanes"]["release"]
assert fast["cleanBuild"] is False
assert release["cleanBuild"] is True
assert fast["artifactPath"] != release["artifactPath"]
assert fast["manifestPath"] != release["manifestPath"]
assert fast["resourceReportPath"] != release["resourceReportPath"]
assert "/fast/" in fast["artifactPath"]
assert "/release/" in release["artifactPath"]
capacity = profile["flash"]["capacityPolicy"]
assert capacity["scope"] == "each-application-slot"
assert capacity["slotSizeBytes"] == 0x300000
assert capacity["greenBelowPercent"] == 75
assert capacity["yellowBelowPercent"] == 80
assert capacity["orangeBelowPercent"] == 85
assert capacity["hardBlockAtOrAbovePercent"] == 85
assert 'OUTPUT_DIR="$WORK_DIR/output/$BUILD_MODE"' in build
assert 'FLASH_MODE=release' in flash
assert 'validate-flash-artifact.py' in flash
assert 'artifact_manifest_binary_sha256' in artifact_validator
assert 'release_artifact_toolchain' in artifact_validator
assert 'artifact_flash_offset' in artifact_validator
identity_authority = profile["discovery"]["identityAuthority"]
assert identity_authority["requiredBeforeDeviceAccess"] is True
assert identity_authority["argument"] == "--identity-authority"
assert "16777216 bytes" in identity_authority["expected"][-1]
assert "--identity-authority {identity_authority}" in fast["flashCommand"]
assert "--identity-authority {identity_authority}" in release["flashCommand"]
assert 'FAIL identity_authority_required' in flash
assert 'CHIP = "ESP32-S3"' in identity_validator
assert 'FLASH_BYTES = 16 * 1024 * 1024' in identity_validator
assert 'device_id_from_mac(observed_mac)' in identity_validator
transfer = profile["flash"]["transfer"]
assert transfer == {
    "resetBefore": "usb-reset",
    "stubPolicy": "disabled",
    "baud": 115200,
    "chunkSizeBytes": 16384,
    "maxAttemptsPerChunk": 3,
    "resumable": True,
    "verification": "full-readback",
}
assert 'HARDMAC_ESP32_TRANSFER' in flash
assert 'hardmac.esp32-region-transfer.v1' in flash
assert '--device-key "$DEVICE_KEY"' in flash
assert '--chunk-size 16384' in flash
assert '--before usb-reset' in flash
assert '--stub disabled' in flash
assert '--max-size 0x300000' in flash
assert 'work/hardmac-runs' in flash
backup_call = 'python3 "$TRANSFER_SCRIPT" backup'
flash_call = 'python3 "$TRANSFER_SCRIPT" flash'
identity_call = 'DEVICE_KEY=$(python3 "$IDENTITY_VALIDATOR" evidence'
assert backup_call in flash
assert flash.index(identity_call) < flash.index(backup_call) < flash.index(flash_call)
assert '--size 0x300000' in flash
assert 'current-app0.bin' in flash
assert 'restore-plan.json' in flash
assert 'write_artifact_manifest' in build
assert 'flash-size-policy.py' in build
assert '--slot-bytes "$APP_SLOT_BYTES"' in build
assert '--enforce' in build
assert '"sourceDirty"' in manifest_writer
assert '"resourcePolicy"' in manifest_writer

serialized = json.dumps(profile, ensure_ascii=False)
for forbidden in ("/dev/cu.", "secretId", "secretKey", "wifiPassword"):
    assert forbidden not in serialized

print("PASS hardmac_workflow")
