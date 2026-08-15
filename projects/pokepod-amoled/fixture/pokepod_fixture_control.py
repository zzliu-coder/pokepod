#!/usr/bin/env python3
"""Fail-closed BOOT/RESET/power controller for the computer-side fixture."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
import subprocess
import time
from typing import Any


SCHEMA = "pokepod.fixture.control.v1"
ACTION_NAMES = (
    "assert_boot",
    "release_boot",
    "pulse_reset",
    "power_off",
    "power_on",
)


class FixtureControlError(RuntimeError):
    pass


def _command(value: object, name: str) -> list[str] | None:
    if value is None:
        return None
    if not isinstance(value, list) or not value or not all(
        isinstance(item, str) and item for item in value
    ):
        raise FixtureControlError(f"action {name} must be a non-empty argv array")
    return list(value)


@dataclass(frozen=True)
class FixtureControlProfile:
    backend: str
    actions: dict[str, list[str] | None]
    action_timeout_seconds: float = 5.0
    boot_settle_seconds: float = 0.10
    reset_settle_seconds: float = 0.35
    power_off_seconds: float = 0.50

    @staticmethod
    def load(path: Path) -> "FixtureControlProfile":
        try:
            raw: Any = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise FixtureControlError(f"cannot read control profile: {path}") from error
        if not isinstance(raw, dict) or raw.get("schema") != SCHEMA:
            raise FixtureControlError(f"control profile schema must be {SCHEMA}")
        backend = raw.get("backend")
        if backend not in ("manual", "command"):
            raise FixtureControlError("control backend must be manual or command")
        raw_actions = raw.get("actions", {})
        if not isinstance(raw_actions, dict):
            raise FixtureControlError("control actions must be an object")
        actions = {name: _command(raw_actions.get(name), name)
                   for name in ACTION_NAMES}
        if backend == "command":
            for required in ("assert_boot", "release_boot", "pulse_reset"):
                if actions[required] is None:
                    raise FixtureControlError(
                        f"command backend requires action {required}"
                    )
            has_power_off = actions["power_off"] is not None
            has_power_on = actions["power_on"] is not None
            if has_power_off != has_power_on:
                raise FixtureControlError(
                    "power_off and power_on must be configured together"
                )
        timings = raw.get("timings", {})
        if not isinstance(timings, dict):
            raise FixtureControlError("control timings must be an object")

        def seconds(key: str, default: float) -> float:
            value = timings.get(key, default)
            if not isinstance(value, (int, float)) or not 0 < value <= 30:
                raise FixtureControlError(f"invalid timing {key}")
            return float(value)

        return FixtureControlProfile(
            backend=backend,
            actions=actions,
            action_timeout_seconds=seconds("actionTimeoutSeconds", 5.0),
            boot_settle_seconds=seconds("bootSettleSeconds", 0.10),
            reset_settle_seconds=seconds("resetSettleSeconds", 0.35),
            power_off_seconds=seconds("powerOffSeconds", 0.50),
        )

    def capabilities(self) -> dict[str, object]:
        return {
            "schema": SCHEMA,
            "backend": self.backend,
            "automaticBootReset": self.backend == "command",
            "automaticPowerCycle": self.backend == "command"
            and self.actions["power_off"] is not None,
        }


class FixtureController:
    def __init__(self, profile: FixtureControlProfile, evidence_dir: Path):
        self.profile = profile
        self.evidence_dir = evidence_dir
        self.evidence_dir.mkdir(parents=True, exist_ok=True)

    def _run(self, name: str) -> None:
        command = self.profile.actions.get(name)
        if self.profile.backend != "command" or command is None:
            raise FixtureControlError(
                f"fixture action {name} needs a configured computer-side controller"
            )
        completed = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=self.profile.action_timeout_seconds,
            check=False,
        )
        (self.evidence_dir / f"control-{name}.stdout").write_text(
            completed.stdout, encoding="utf-8"
        )
        (self.evidence_dir / f"control-{name}.stderr").write_text(
            completed.stderr, encoding="utf-8"
        )
        if completed.returncode != 0:
            raise FixtureControlError(
                f"fixture action {name} failed with exit {completed.returncode}"
            )

    def enter_rom_loader(self) -> None:
        boot_asserted = False
        try:
            self._run("assert_boot")
            boot_asserted = True
            time.sleep(self.profile.boot_settle_seconds)
            self._run("pulse_reset")
            time.sleep(self.profile.reset_settle_seconds)
        finally:
            if boot_asserted:
                self._run("release_boot")

    def power_cycle(self) -> None:
        if self.profile.actions["power_off"] is None:
            raise FixtureControlError("fixture controller has no power switching")
        self._run("power_off")
        time.sleep(self.profile.power_off_seconds)
        self._run("power_on")
        time.sleep(self.profile.reset_settle_seconds)

