#!/usr/bin/env python3
"""Behavior test for fail-closed fixture BOOT/RESET sequencing."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "fixture" / "pokepod_fixture_control.py"
SPEC = importlib.util.spec_from_file_location("pokepod_fixture_control", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def write_profile(path: Path, helper: Path, state: Path,
                  fail_on: str = "") -> None:
    def action(name: str) -> list[str]:
        command = [sys.executable, str(helper), str(state), name]
        if fail_on:
            command.append(fail_on)
        return command

    path.write_text(json.dumps({
        "schema": MODULE.SCHEMA,
        "backend": "command",
        "actions": {
            "ping": action("ping"),
            "assert_boot": action("assert_boot"),
            "release_boot": action("release_boot"),
            "release_reset": action("release_reset"),
            "pulse_reset": action("pulse_reset"),
            "power_off": action("power_off"),
            "power_on": action("power_on"),
        },
        "timings": {
            "actionTimeoutSeconds": 1,
            "bootSettleSeconds": 0.001,
            "resetSettleSeconds": 0.001,
            "powerOffSeconds": 0.001,
        },
    }), encoding="utf-8")


def main() -> int:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        helper = root / "helper.py"
        helper.write_text(
            "from pathlib import Path\n"
            "import sys\n"
            "path=Path(sys.argv[1])\n"
            "name=sys.argv[2]\n"
            "path.write_text((path.read_text() if path.exists() else '') + name + '\\n')\n"
            "raise SystemExit(7 if len(sys.argv)>3 and sys.argv[3] == name else 0)\n",
            encoding="utf-8",
        )
        profile_path = root / "profile.json"
        state = root / "state.txt"
        write_profile(profile_path, helper, state)
        profile = MODULE.FixtureControlProfile.load(profile_path)
        assert profile.capabilities()["automaticBootReset"] is True
        assert profile.capabilities()["automaticPowerCycle"] is True
        controller = MODULE.FixtureController(profile, root / "evidence")
        controller.enter_rom_loader()
        controller.power_cycle()
        assert state.read_text().splitlines() == [
            "assert_boot", "pulse_reset", "release_boot",
            "power_off", "power_on",
        ]

        state.unlink()
        write_profile(profile_path, helper, state, "pulse_reset")
        failing = MODULE.FixtureController(
            MODULE.FixtureControlProfile.load(profile_path), root / "failure"
        )
        try:
            failing.enter_rom_loader()
            raise AssertionError("pulse failure must propagate")
        except MODULE.FixtureControlError:
            pass
        assert state.read_text().splitlines() == [
            "assert_boot", "pulse_reset", "release_boot"
        ]

        manual_path = root / "manual.json"
        manual_path.write_text(json.dumps({
            "schema": MODULE.SCHEMA, "backend": "manual", "actions": {}
        }), encoding="utf-8")
        manual = MODULE.FixtureControlProfile.load(manual_path)
        assert manual.capabilities()["automaticBootReset"] is False

    print("PASS test-fixture-control")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
