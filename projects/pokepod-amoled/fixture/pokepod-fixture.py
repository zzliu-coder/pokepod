#!/usr/bin/env python3
"""Computer-side PokePod fixture: identity, diagnostics, OTA and rescue flash."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time


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


def update(port: str, firmware: Path, timeout: float) -> int:
    if not firmware.is_file():
        raise RuntimeError(f"firmware image does not exist: {firmware}")
    image = firmware.read_bytes()
    if not 1024 <= len(image) <= 0x300000:
        raise RuntimeError("firmware image must be between 1 KiB and 3 MiB")
    output = run_dir("update")
    metadata = {"port": port, "firmware": str(firmware), "bytes": len(image),
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
    time.sleep(2.0)
    evidence: dict[str, object] = {"request": metadata, "response": completed.stdout}
    try:
        evidence["post"] = {"status": cdc(port, "status", output, timeout)}
    except RuntimeError as error:
        evidence["post"] = {"error": str(error)}
    write_json(output / "evidence.json", evidence)
    print(output)
    return 0


def rescue_flash(args: argparse.Namespace) -> int:
    output = run_dir("flash")
    command = [str(FLASH), f"--{args.mode}", "--rom-port", args.rom_port]
    if args.authority:
        command.extend(["--identity-authority", str(args.authority)])
    completed = subprocess.run(command, cwd=PROJECT, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               check=False)
    (output / "flash.stdout").write_text(completed.stdout, encoding="utf-8")
    (output / "flash.stderr").write_text(completed.stderr, encoding="utf-8")
    if completed.returncode != 0:
        raise RuntimeError(f"rescue flash failed; evidence={output}")
    print(output)
    return 0


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
    flash_parser = sub.add_parser("flash")
    flash_parser.add_argument("--rom-port", required=True)
    flash_parser.add_argument("--authority", type=Path)
    flash_parser.add_argument("--mode", choices=("fast", "release"), default="fast")
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
            return update(args.port, args.firmware, args.timeout)
        return rescue_flash(args)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"fixture failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
