#!/usr/bin/env python3
"""Computer-side PokePod fixture: identity, diagnostics, OTA and rescue flash."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import glob
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

from pokepod_fixture_control import (
    FixtureControlError,
    FixtureController,
    FixtureControlProfile,
)


PROJECT = Path(__file__).resolve().parents[0].parent
CDC = PROJECT / "cdc-status.py"
FLASH = PROJECT / "flash.sh"
RUNS = PROJECT / "work" / "fixture-runs"


def run_dir(operation: str) -> Path:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    path = RUNS / f"{stamp}-{operation}"
    path.mkdir(parents=True, exist_ok=False)
    return path


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n")


def cdc(port: str, command: str, output: Path, timeout: float) -> dict[str, object]:
    completed = subprocess.run(
        [sys.executable, str(CDC), port, "--command", command, "--timeout", str(timeout)],
        cwd=PROJECT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=max(timeout + 5.0, 10.0),
        check=False,
    )
    (output / f"cdc-{command}.stdout").write_text(completed.stdout, encoding="utf-8")
    (output / f"cdc-{command}.stderr").write_text(completed.stderr, encoding="utf-8")
    if completed.returncode != 0:
        raise RuntimeError(f"CDC {command} failed: {completed.stderr.strip()}")
    try:
        value = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"CDC {command} returned non-JSON output") from error
    write_json(output / f"cdc-{command}.json", value)
    return value


def matching_ports(pattern: str) -> list[str]:
    return sorted(path for path in glob.glob(pattern) if Path(path).exists())


def wait_for_unique_port(pattern: str, deadline_seconds: float) -> str:
    deadline = time.monotonic() + deadline_seconds
    last: list[str] = []
    while time.monotonic() < deadline:
        last = matching_ports(pattern)
        if len(last) == 1:
            return last[0]
        time.sleep(0.20)
    raise RuntimeError(
        f"expected one port matching {pattern}, observed {len(last)}: {last}"
    )


def wait_for_application(
    pattern: str, expected_device_id: str, output: Path,
    deadline_seconds: float, command_timeout: float,
) -> tuple[str, dict[str, object]]:
    deadline = time.monotonic() + deadline_seconds
    last_errors: dict[str, str] = {}
    while time.monotonic() < deadline:
        for port in matching_ports(pattern):
            try:
                identity = cdc(port, "hello", output, command_timeout)
            except RuntimeError as error:
                last_errors[port] = str(error)
                continue
            observed = str(identity.get("deviceId", ""))
            if observed == expected_device_id:
                return port, identity
            last_errors[port] = f"unexpected deviceId {observed}"
        time.sleep(0.35)
    raise RuntimeError(
        f"application did not return as {expected_device_id}; observations={last_errors}"
    )


def collect(port: str, operation: str, timeout: float) -> int:
    output = run_dir(operation)
    identity = cdc(port, "hello", output, timeout)
    status = cdc(port, "status", output, timeout)
    diagnostics: dict[str, object] = {"identity": identity, "status": status}
    for command in ("get-runtime-diagnostics", "get-power-diagnostics",
                    "get-provisioning-diagnostics"):
        try:
            diagnostics[command] = cdc(port, command, output, timeout)
        except RuntimeError as error:
            diagnostics[command] = {"error": str(error)}
    write_json(output / "evidence.json", diagnostics)
    print(output)
    return 0


def update(port: str, firmware: Path, timeout: float, port_pattern: str,
           app_timeout: float) -> int:
    if not firmware.is_file():
        raise RuntimeError(f"firmware image does not exist: {firmware}")
    image = firmware.read_bytes()
    if not 1024 <= len(image) <= 0x300000:
        raise RuntimeError("firmware image must be between 1 KiB and 3 MiB")
    output = run_dir("update")
    before = cdc(port, "hello", output, min(timeout, 5.0))
    expected_device_id = str(before.get("deviceId", ""))
    if not expected_device_id.startswith("pokepod-"):
        raise RuntimeError("USB update refused unexpected Link identity")
    metadata = {"port": port, "deviceId": expected_device_id,
                "firmware": str(firmware), "bytes": len(image),
                "sha256": hashlib.sha256(image).hexdigest()}
    write_json(output / "request.json", metadata)
    command = [sys.executable, str(CDC), port, "--firmware", str(firmware),
               "--timeout", str(timeout)]
    completed = subprocess.run(command, cwd=PROJECT, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               timeout=max(timeout + len(image) / 4096.0 + 30.0, 60.0),
                               check=False)
    (output / "update.stdout").write_text(completed.stdout, encoding="utf-8")
    (output / "update.stderr").write_text(completed.stderr, encoding="utf-8")
    if completed.returncode != 0:
        raise RuntimeError(f"USB firmware update failed: {completed.stderr.strip()}")
    app_port, post_identity = wait_for_application(
        port_pattern, expected_device_id, output, app_timeout,
        min(timeout, 5.0),
    )
    evidence: dict[str, object] = {
        "request": metadata,
        "response": completed.stdout,
        "post": {
            "port": app_port,
            "identity": post_identity,
            "status": cdc(app_port, "status", output, min(timeout, 5.0)),
        },
    }
    write_json(output / "evidence.json", evidence)
    print(output)
    return 0


def load_controller(profile_path: Path, output: Path) -> FixtureController:
    profile = FixtureControlProfile.load(profile_path)
    write_json(output / "control-capabilities.json", profile.capabilities())
    if profile.backend != "command":
        raise RuntimeError(
            "control profile is manual; wire a command backend before automatic recovery"
        )
    return FixtureController(profile, output)


def flash_with_evidence(
    output: Path, rom_port: str, authority: Path | None, mode: str,
) -> None:
    command = [str(FLASH), f"--{mode}", "--rom-port", rom_port]
    if authority:
        command.extend(["--identity-authority", str(authority)])
    completed = subprocess.run(
        command, cwd=PROJECT, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False,
    )
    (output / "flash.stdout").write_text(completed.stdout, encoding="utf-8")
    (output / "flash.stderr").write_text(completed.stderr, encoding="utf-8")
    if completed.returncode != 0:
        raise RuntimeError(f"rescue flash failed; evidence={output}")


def rescue_flash(args: argparse.Namespace) -> int:
    output = run_dir("flash")
    flash_with_evidence(output, args.rom_port, args.authority, args.mode)
    print(output)
    return 0


def recover(args: argparse.Namespace) -> int:
    output = run_dir("recover")
    controller = load_controller(args.control_profile, output)
    controller.enter_rom_loader()
    rom_port = wait_for_unique_port(args.port_pattern, args.port_timeout)
    write_json(output / "rom-port.json", {"port": rom_port})
    flash_with_evidence(output, rom_port, args.authority, args.mode)
    if args.expected_device_id:
        app_port, identity = wait_for_application(
            args.port_pattern, args.expected_device_id, output,
            args.app_timeout, args.timeout,
        )
        status = cdc(app_port, "status", output, args.timeout)
        write_json(output / "post-flash.json", {
            "port": app_port,
            "identity": identity,
            "status": status,
        })
    print(output)
    return 0


def exercise(args: argparse.Namespace) -> int:
    output = run_dir(f"exercise-{args.scenario}")
    identity = cdc(args.port, "hello", output, args.timeout)
    expected_device_id = str(identity.get("deviceId", ""))
    if not expected_device_id.startswith("pokepod-"):
        raise RuntimeError("fixture exercise refused unexpected Link identity")
    write_json(output / "pre.json", {
        "identity": identity,
        "status": cdc(args.port, "status", output, args.timeout),
    })
    failure: RuntimeError | None = None
    try:
        if args.scenario == "recording":
            cdc(args.port, "record", output, args.timeout)
            time.sleep(args.hold_seconds)
            cdc(args.port, "stop", output, args.timeout)
        elif args.scenario == "provisioning":
            cdc(args.port, "provisioning-start", output, args.timeout)
            time.sleep(args.hold_seconds)
            cdc(args.port, "provisioning-stop", output, args.timeout)
        else:
            raise RuntimeError(f"unsupported scenario {args.scenario}")
        time.sleep(1.0)
        app_port, post_identity = wait_for_application(
            args.port_pattern, expected_device_id, output,
            args.app_timeout, args.timeout,
        )
        write_json(output / "post.json", {
            "port": app_port,
            "identity": post_identity,
            "status": cdc(app_port, "status", output, args.timeout),
            "runtime": cdc(app_port, "get-runtime-diagnostics", output,
                           args.timeout),
        })
    except RuntimeError as error:
        failure = error
    if failure is None:
        write_json(output / "result.json", {
            "scenario": args.scenario, "passed": True,
            "automaticRecovery": False,
        })
        print(output)
        return 0
    write_json(output / "scenario-failure.json", {"error": str(failure)})
    if not args.auto_recover or args.control_profile is None:
        raise failure
    controller = load_controller(args.control_profile, output)
    controller.enter_rom_loader()
    rom_port = wait_for_unique_port(args.port_pattern, args.port_timeout)
    flash_with_evidence(output, rom_port, args.authority, args.mode)
    app_port, post_identity = wait_for_application(
        args.port_pattern, expected_device_id, output,
        args.app_timeout, args.timeout,
    )
    write_json(output / "result.json", {
        "scenario": args.scenario,
        "passed": False,
        "automaticRecovery": True,
        "recovered": True,
        "port": app_port,
        "identity": post_identity,
    })
    print(output)
    return 3


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="operation", required=True)
    probe = sub.add_parser("probe")
    probe.add_argument("--port", required=True)
    probe.add_argument("--timeout", type=float, default=3.0)
    collect_parser = sub.add_parser("collect")
    collect_parser.add_argument("--port", required=True)
    collect_parser.add_argument("--timeout", type=float, default=3.0)
    update_parser = sub.add_parser("update")
    update_parser.add_argument("--port", required=True)
    update_parser.add_argument("--firmware", type=Path, required=True)
    update_parser.add_argument(
        "--timeout", type=float, default=60.0,
        help="per-chunk Link timeout; the first OTA prepare may erase the inactive slot",
    )
    update_parser.add_argument("--port-pattern", default="/dev/cu.usbmodem*")
    update_parser.add_argument("--app-timeout", type=float, default=30.0)
    flash_parser = sub.add_parser("flash")
    flash_parser.add_argument("--rom-port", required=True)
    flash_parser.add_argument("--authority", type=Path)
    flash_parser.add_argument("--mode", choices=("fast", "release"), default="fast")
    doctor_parser = sub.add_parser("doctor")
    doctor_parser.add_argument("--control-profile", type=Path, required=True)
    recover_parser = sub.add_parser("recover")
    recover_parser.add_argument("--control-profile", type=Path, required=True)
    recover_parser.add_argument("--port-pattern", default="/dev/cu.usbmodem*")
    recover_parser.add_argument("--port-timeout", type=float, default=8.0)
    recover_parser.add_argument("--app-timeout", type=float, default=20.0)
    recover_parser.add_argument("--timeout", type=float, default=3.0)
    recover_parser.add_argument("--authority", type=Path, required=True)
    recover_parser.add_argument("--expected-device-id")
    recover_parser.add_argument("--mode", choices=("fast", "release"), default="fast")
    exercise_parser = sub.add_parser("exercise")
    exercise_parser.add_argument("--scenario", choices=("recording", "provisioning"), required=True)
    exercise_parser.add_argument("--port", required=True)
    exercise_parser.add_argument("--port-pattern", default="/dev/cu.usbmodem*")
    exercise_parser.add_argument("--timeout", type=float, default=3.0)
    exercise_parser.add_argument("--hold-seconds", type=float, default=2.0)
    exercise_parser.add_argument("--app-timeout", type=float, default=20.0)
    exercise_parser.add_argument("--port-timeout", type=float, default=8.0)
    exercise_parser.add_argument("--auto-recover", action="store_true")
    exercise_parser.add_argument("--control-profile", type=Path)
    exercise_parser.add_argument("--authority", type=Path)
    exercise_parser.add_argument("--mode", choices=("fast", "release"), default="fast")
    args = parser.parse_args()
    try:
        if args.operation == "probe":
            output = run_dir("probe")
            write_json(output / "evidence.json", {"hello": cdc(args.port, "hello", output, args.timeout),
                                                   "status": cdc(args.port, "status", output, args.timeout)})
            print(output)
            return 0
        if args.operation == "collect":
            return collect(args.port, "collect", args.timeout)
        if args.operation == "update":
            return update(args.port, args.firmware, args.timeout,
                          args.port_pattern, args.app_timeout)
        if args.operation == "doctor":
            output = run_dir("doctor")
            profile = FixtureControlProfile.load(args.control_profile)
            write_json(output / "evidence.json", profile.capabilities())
            print(output)
            return 0 if profile.backend == "command" else 4
        if args.operation == "recover":
            return recover(args)
        if args.operation == "exercise":
            return exercise(args)
        return rescue_flash(args)
    except (OSError, RuntimeError, FixtureControlError,
            subprocess.TimeoutExpired) as error:
        print(f"fixture failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
