#!/usr/bin/env python3
"""Contract test for the computer-side fixture boundary."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    pinout = json.loads((ROOT / "fixture" / "pinout.json").read_text())
    script = (ROOT / "fixture" / "pokepod-fixture.py").read_text()
    control = (ROOT / "fixture" / "pokepod_fixture_control.py").read_text()
    bridge = (ROOT / "fixture" / "pokepod-fixture-bridge.py").read_text()
    controller = (ROOT / "fixture" / "controller-firmware" /
                  "pokepod_fixture_controller.ino").read_text()
    readme = (ROOT / "fixture" / "README.md").read_text()
    contacts = pinout["contacts"]
    for name in ("usb_dp", "usb_dm", "gnd", "vbus_5v", "boot", "reset"):
        assert name in contacts and contacts[name]["required"]
    assert pinout["normal_upgrade"]["requires_manual_boot_reset"] is False
    assert pinout["normal_upgrade"]["artifact_identity_required"] is True
    assert pinout["normal_upgrade"]["rejects_unbound_binary"] is True
    assert pinout["normal_upgrade"]["artifact_identity_fields"] == [
        "sourceRevision", "firmwareVersion", "appElfSha256"
    ]
    assert pinout["rescue_upgrade"]["requires_manual_boot_reset"] is False
    assert pinout["rescue_upgrade"]["requires_fixture_controller"] is True
    assert "identity" in script and "evidence.json" in script
    assert "def link_identity" in script
    assert "expected_build_from_artifact" in script
    assert "require_expected_build" in script
    assert "cdc-status.py" in script and "flash.sh" in script
    assert "--firmware" in script and "--rom-port" in script
    assert "--identity-authority" in script
    assert "fixture-runs" in script
    assert "enter_rom_loader" in script and "wait_for_application" in script
    assert "exercise" in script and "automaticRecovery" in script
    assert "assert_boot" in control and "release_boot" in control
    assert '"ping"' in control and '"release_reset"' in control
    assert "def doctor" in control
    assert "finally:" in control
    assert "--action" in bridge and "serial.Serial" in bridge
    assert "BOOT ASSERT" in controller and "RESET PULSE" in controller
    assert "RESET RELEASE" in controller
    assert "kBootDeadmanMs = 3000" in controller
    assert "OUTPUT LOW" in controller
    assert "normal" in readme.lower() and "rescue" in readme.lower()
    assert "backup" in readme and "回读" in readme
    assert "artifact.json" in readme and "appElfSha256" in readme
    print("PASS test-pokepod-fixture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
