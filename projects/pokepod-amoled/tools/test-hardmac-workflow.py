#!/usr/bin/env python3
"""Keep PokePod's Hard Mac profile and build lanes safely separated."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
profile = json.loads((ROOT / ".hardmac/workflow.json").read_text(encoding="utf-8"))
build = (ROOT / "firmware/build.sh").read_text(encoding="utf-8")
flash = (ROOT / "flash.sh").read_text(encoding="utf-8")
manifest_writer = (ROOT / "tools/write-artifact-manifest.py").read_text(encoding="utf-8")

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
assert "/fast/" in fast["artifactPath"]
assert "/release/" in release["artifactPath"]
assert 'OUTPUT_DIR="$WORK_DIR/output/$BUILD_MODE"' in build
assert 'FLASH_MODE=release' in flash
assert 'artifact_manifest_binary_sha256' in flash
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
assert 'write_artifact_manifest' in build
assert '"sourceDirty"' in manifest_writer

serialized = json.dumps(profile, ensure_ascii=False)
for forbidden in ("/dev/cu.", "secretId", "secretKey", "wifiPassword"):
    assert forbidden not in serialized

print("PASS hardmac_workflow")
