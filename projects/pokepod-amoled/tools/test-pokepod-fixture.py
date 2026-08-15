#!/usr/bin/env python3
"""Contract test for the computer-side fixture boundary."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    pinout = json.loads((ROOT / "fixture" / "pinout.json").read_text())
    script = (ROOT / "fixture" / "pokepod-fixture.py").read_text()
    readme = (ROOT / "fixture" / "README.md").read_text()
    contacts = pinout["contacts"]
    for name in ("usb_dp", "usb_dm", "gnd", "vbus_5v", "boot", "reset"):
        assert name in contacts and contacts[name]["required"]
    assert pinout["normal_upgrade"]["requires_manual_boot_reset"] is False
    assert pinout["rescue_upgrade"]["requires_manual_boot_reset"] is True
    assert "identity" in script and "evidence.json" in script
    assert "cdc-status.py" in script and "flash.sh" in script
    assert "--firmware" in script and "--rom-port" in script
    assert "--identity-authority" in script
    assert "fixture-runs" in script
    assert "normal" in readme.lower() and "rescue" in readme.lower()
    assert "backup" in readme and "回读" in readme
    print("PASS test-pokepod-fixture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
