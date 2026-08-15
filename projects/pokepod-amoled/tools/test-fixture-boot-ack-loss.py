#!/usr/bin/env python3
"""Prove BOOT release still runs when ASSERT executes but ACK is lost."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CONTROL_PATH = ROOT / "fixture" / "pokepod_fixture_control.py"
spec = importlib.util.spec_from_file_location("fixture_control", CONTROL_PATH)
assert spec and spec.loader
control = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = control
spec.loader.exec_module(control)

HELPER = r"""from pathlib import Path
import sys
name = sys.argv[1]
root = Path(sys.argv[2])
(root / f"{name}.ran").write_text("1", encoding="utf-8")
if name == "assert_boot":
    raise SystemExit(7)
"""


def main() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        helper = root / "action.py"
        helper.write_text(HELPER, encoding="utf-8")
        actions = {
            name: [sys.executable, str(helper), name, str(root)]
            for name in control.ACTION_NAMES
        }
        profile_path = root / "control.json"
        profile_path.write_text(json.dumps({
            "schema": control.SCHEMA,
            "backend": "command",
            "actions": actions,
            "timings": {
                "actionTimeoutSeconds": 1.0,
                "bootSettleSeconds": 0.01,
                "resetSettleSeconds": 0.01,
                "powerOffSeconds": 0.01,
            },
        }), encoding="utf-8")
        profile = control.FixtureControlProfile.load(profile_path)
        controller = control.FixtureController(profile, root / "evidence")
        try:
            controller.enter_rom_loader()
        except control.FixtureControlError:
            pass
        else:
            raise AssertionError("lost BOOT ASSERT ACK did not fail")
        assert (root / "assert_boot.ran").is_file()
        assert (root / "release_boot.ran").is_file()
    print("PASS fixture_boot_ack_loss_release")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
