#!/usr/bin/env python3
"""Contract test for the computer-side fixture boundary."""

import json
import importlib.util
from pathlib import Path
import sys


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
    assert "def diagnose" in script and "def diagnose_rom" in script
    assert '"get-runtime-trace"' in script
    assert '"coredump"' in script and '"nvs.private"' in script
    assert "copy_mac_voice_diagnostic" in script
    assert "def latest_boot" in script and "def require_same_boot" in script
    assert 'require_same_boot(pre_boot, mid_boot, "active provisioning")' in script
    assert 'require_same_boot(pre_boot, post_boot, "recording")' in script
    assert 'post_boot["resetReason"] != 3' in script
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
    assert "diagnose" in readme and "64 条 RAM runtime trace" in readme

    fixture_dir = ROOT / "fixture"
    sys.path.insert(0, str(fixture_dir))
    spec = importlib.util.spec_from_file_location(
        "pokepod_fixture", fixture_dir / "pokepod-fixture.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    boot = module.latest_boot({"records": [
        {"subsystem": "recording", "stage": "recording_started",
         "sequence": 42, "reset_reason": 0},
        {"subsystem": "boot", "stage": "boot",
         "sequence": 41, "reset_reason": 4},
    ]})
    assert boot == {"sequence": 41, "resetReason": 4}
    module.require_same_boot(boot, dict(boot), "test")
    try:
        module.require_same_boot(
            boot, {"sequence": 42, "resetReason": 3}, "test")
    except RuntimeError as error:
        assert "unexpected device restart during test" in str(error)
    else:
        raise AssertionError("fixture restart check accepted a changed boot")
    print("PASS test-pokepod-fixture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
