#!/usr/bin/env python3
"""Exercise V1/V2 evidence generation without opening a device."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import tempfile


PROJECT = Path(__file__).resolve().parents[1]
SCRIPT = PROJECT / "device-acceptance.sh"
SOURCE = SCRIPT.read_text(encoding="utf-8")

assert "--expected-variant" in SOURCE
assert "--evidence-root" in SOURCE
assert "cdc-status.py\" \"$PORT\"" in SOURCE
assert "--command identity" in SOURCE
assert "glob" not in SOURCE
assert "flash.sh" not in SOURCE
assert "esptool" not in SOURCE
assert "pokepod.device-acceptance.evidence.v2" in SOURCE

with tempfile.TemporaryDirectory(prefix="pokepod-device-acceptance-") as raw:
    root = Path(raw)
    missing = subprocess.run(
        [str(SCRIPT), "--evidence-root", str(root)],
        text=True, capture_output=True,
    )
    assert missing.returncode != 0

    for key, name in (("v1", "V1 SH8601/FT3168"), ("v2", "V2 CO5300/CST820")):
        fixture = root / f"{key}.json"
        fixture.write_text(json.dumps({"variant": name, "deviceId": "fixture-device"}))
        result = subprocess.run(
            [str(SCRIPT), "--expected-variant", key, "--evidence-root", str(root),
             "--status-fixture", str(fixture), "--dry-run"],
            text=True, capture_output=True,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        manifests = list((root / key).glob("*/evidence.json"))
        assert len(manifests) == 1
        payload = json.loads(manifests[0].read_text())
        assert payload["schema"] == "pokepod.device-acceptance.evidence.v2"
        assert payload["expectedVariant"] == key
        assert payload["overall"] == "unverified"
        assert payload["writesFirmware"] is False
        assert payload["autoSelectsSerial"] is False
        assert payload["preflight"]["status"] == "unverified"
        assert payload["preflight"]["rawEvidence"] == [
            "preflight-status.json", "preflight-identity.json"
        ]
        assert all(scenario["status"] == "unverified" for scenario in payload["scenarios"])

    mismatch = root / "mismatch.json"
    mismatch.write_text(json.dumps({"variant": "V2 CO5300/CST820", "deviceId": "x"}))
    failed = subprocess.run(
        [str(SCRIPT), "--expected-variant", "v1", "--evidence-root", str(root),
         "--status-fixture", str(mismatch), "--dry-run"],
        text=True, capture_output=True,
    )
    assert failed.returncode != 0

print("PASS device_acceptance_contract")
