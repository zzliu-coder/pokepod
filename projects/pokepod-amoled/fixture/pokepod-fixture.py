#!/usr/bin/env python3
"""Computer-side PokePod fixture: identity, diagnostics, OTA and rescue flash."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import glob
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
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
SOURCE_REVISION_RE = re.compile(r"^[0-9a-fA-F]{40}$")
APP_ELF_SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
MAC_VOICE_DIAGNOSTIC = (
    Path.home() / "Library" / "Application Support" /
    "PokePodVoice" / "last-session.json"
)


def run_dir(operation: str) -> Path:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    path = RUNS / f"{stamp}-{operation}"
    path.mkdir(parents=True, exist_ok=False)
    return path


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n")


def load_firmware_artifact_identity(firmware: Path) -> dict[str, str]:
    """Load the product identity bound to a firmware image's artifact."""
    manifest_path = firmware.with_name("artifact.json")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise RuntimeError(
            f"identity-bound USB update requires adjacent artifact.json: {manifest_path}"
        ) from error
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"invalid adjacent artifact.json: {manifest_path}") from error
    if not isinstance(manifest, dict):
        raise RuntimeError(f"invalid artifact.json: {manifest_path}")
    if manifest.get("schemaVersion") != 1 or manifest.get("kind") != "hardmac.artifact":
        raise RuntimeError("adjacent artifact.json has an invalid schema")
    if manifest.get("sourceDirty") is not False:
        raise RuntimeError("firmware artifact is dirty; refusing USB update")
    source_revision = manifest.get("sourceRevision")
    firmware_version = manifest.get("firmwareVersion")
    image_identity = manifest.get("imageIdentity")
    if not isinstance(source_revision, str) or SOURCE_REVISION_RE.fullmatch(source_revision) is None:
        raise RuntimeError("artifact sourceRevision is missing or invalid")
    if (not isinstance(firmware_version, str) or not (1 <= len(firmware_version) <= 15)
            or any(ord(character) < 0x20 or ord(character) >= 0x7F
                   for character in firmware_version)):
        raise RuntimeError("artifact firmwareVersion is missing or invalid")
    if not isinstance(image_identity, dict):
        raise RuntimeError("artifact imageIdentity is missing")
    if image_identity.get("magic") != "PKPDIMG2" or image_identity.get("schema") != 2:
        raise RuntimeError("artifact imageIdentity schema is invalid")
    if image_identity.get("product") != "PokePodAmoled":
        raise RuntimeError("artifact imageIdentity product is invalid")
    if image_identity.get("sourceRevision") != source_revision:
        raise RuntimeError("artifact imageIdentity sourceRevision mismatch")
    if image_identity.get("firmwareVersion") != firmware_version:
        raise RuntimeError("artifact imageIdentity firmwareVersion mismatch")
    if image_identity.get("sourceDirty") is not False:
        raise RuntimeError("artifact imageIdentity is dirty")
    app_elf_sha256 = image_identity.get("appElfSha256")
    if not isinstance(app_elf_sha256, str) or APP_ELF_SHA256_RE.fullmatch(app_elf_sha256) is None:
        raise RuntimeError("artifact imageIdentity appElfSha256 is missing or invalid")
    binary = manifest.get("binary")
    if not isinstance(binary, dict) or binary.get("file") != firmware.name:
        raise RuntimeError("artifact binary does not match firmware image")
    payload = firmware.read_bytes()
    if binary.get("sizeBytes") != len(payload):
        raise RuntimeError("artifact binary size does not match firmware image")
    if binary.get("sha256") != hashlib.sha256(payload).hexdigest():
        raise RuntimeError("artifact binary SHA-256 does not match firmware image")
    return {
        "sourceRevision": source_revision.lower(),
        "firmwareVersion": firmware_version,
        "appElfSha256": app_elf_sha256.lower(),
    }


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


def link_identity(
    port: str, output: Path, timeout: float,
) -> dict[str, object]:
    """Read the permanent device identity through its dedicated operation.

    ``hello`` is deliberately kept as a capability probe.  It may be
    answered by a bootloader or a compatibility endpoint and therefore must
    never become the authority for pairing a fixture run to a physical unit.
    """
    identity = cdc(port, "identity", output, timeout)
    device_id = str(identity.get("deviceId", ""))
    if not device_id.startswith("pokepod-"):
        raise RuntimeError("PokePod identity response is missing deviceId")
    return identity


def expected_build_from_artifact(firmware: Path) -> dict[str, object]:
    """Validate a closed artifact and derive the build identity to expect.

    The fixture accepts no caller-provided identity fields.  All values come
    from the manifest and the bytes being sent, and validation happens before
    the first Link frame is opened.
    """
    manifest_path = firmware.parent / "artifact.json"
    validator = PROJECT / "tools" / "validate-flash-artifact.py"
    completed = subprocess.run(
        [sys.executable, str(validator), "--manifest", str(manifest_path),
         "--binary", str(firmware)],
        cwd=PROJECT, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=30.0, check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "firmware artifact validation failed: " +
            (completed.stderr.strip() or completed.stdout.strip())
        )
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(
            f"firmware requires adjacent validated artifact.json: {manifest_path}"
        ) from error
    binary = manifest.get("binary")
    if not isinstance(binary, dict):
        raise RuntimeError("firmware artifact is missing binary metadata")
    payload = firmware.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    if (binary.get("file") != firmware.name or
            binary.get("sizeBytes") != len(payload) or
            binary.get("sha256") != digest):
        raise RuntimeError("firmware does not match adjacent artifact.json")
    # ESP image descriptor fields are stable across the supported Arduino
    # core versions.  This check is intentionally small and fail-closed.
    if len(payload) < 208 or int.from_bytes(payload[32:36], "little") != 0xABCD5432:
        raise RuntimeError("firmware has no valid ESP application descriptor")
    source_revision = manifest.get("sourceRevision")
    firmware_version = manifest.get("firmwareVersion")
    if (not isinstance(source_revision, str) or len(source_revision) != 40 or
            not isinstance(firmware_version, str) or not firmware_version):
        raise RuntimeError("firmware artifact has no exact build identity")
    return {
        "sourceRevision": source_revision,
        "firmwareVersion": firmware_version,
        "appElfSha256": payload[176:208].hex(),
        "binarySha256": digest,
    }


def require_expected_build(
    identity: dict[str, object], expected: dict[str, object],
) -> None:
    for key in ("sourceRevision", "firmwareVersion", "appElfSha256"):
        if identity.get(key) != expected.get(key):
            raise RuntimeError(
                f"running firmware {key} mismatch: "
                f"expected={expected.get(key)!r} actual={identity.get(key)!r}"
            )
    if identity.get("sourceDirty") is not False:
        raise RuntimeError("running firmware is not a clean exact candidate")
    if identity.get("runningPartition") not in ("app0", "app1"):
        raise RuntimeError("running firmware did not report an OTA app partition")


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
                identity = link_identity(port, output, command_timeout)
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


def collect_into(output: Path, port: str, timeout: float) -> dict[str, object]:
    hello = cdc(port, "hello", output, timeout)
    identity = link_identity(port, output, timeout)
    status = cdc(port, "status", output, timeout)
    diagnostics: dict[str, object] = {
        "hello": hello, "identity": identity, "status": status
    }
    for command in ("get-runtime-diagnostics", "get-power-diagnostics",
                    "get-provisioning-diagnostics"):
        try:
            diagnostics[command] = cdc(port, command, output, timeout)
        except RuntimeError as error:
            diagnostics[command] = {"error": str(error)}
    trace_records: list[object] = []
    trace_offset = 0
    while trace_offset < 64:
        completed = subprocess.run(
            [sys.executable, str(CDC), port, "--command",
             "get-runtime-trace", "--trace-offset", str(trace_offset),
             "--trace-limit", "8", "--timeout", str(timeout)],
            cwd=PROJECT, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=max(timeout + 5.0, 10.0),
            check=False,
        )
        (output / f"cdc-runtime-trace-{trace_offset}.stdout").write_text(
            completed.stdout, encoding="utf-8")
        (output / f"cdc-runtime-trace-{trace_offset}.stderr").write_text(
            completed.stderr, encoding="utf-8")
        if completed.returncode != 0:
            diagnostics["get-runtime-trace"] = {
                "error": completed.stderr.strip(), "records": trace_records}
            break
        page = json.loads(completed.stdout)
        records = page.get("records")
        if isinstance(records, list):
            trace_records.extend(records)
        next_offset = page.get("next_offset")
        if not isinstance(next_offset, int):
            diagnostics["get-runtime-trace"] = {
                "status": "ok", "total": page.get("total", len(trace_records)),
                "records": trace_records}
            break
        trace_offset = next_offset
    return diagnostics


def collect(port: str, operation: str, timeout: float) -> int:
    output = run_dir(operation)
    diagnostics = collect_into(output, port, timeout)
    write_json(output / "evidence.json", diagnostics)
    print(output)
    return 0


def copy_mac_voice_diagnostic(output: Path) -> dict[str, object]:
    target = output / "mac-voice-last-session.json"
    if not MAC_VOICE_DIAGNOSTIC.is_file():
        return {"status": "unavailable", "path": str(MAC_VOICE_DIAGNOSTIC)}
    shutil.copyfile(MAC_VOICE_DIAGNOSTIC, target)
    return {
        "status": "captured",
        "file": target.name,
        "sizeBytes": target.stat().st_size,
        "sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
    }


def find_esptool() -> Path:
    configured = os.environ.get("ESPTOOL_BIN")
    if configured:
        candidate = Path(configured)
        if candidate.is_file():
            return candidate
    candidates = sorted((Path.home() / "Library" / "Arduino15" / "packages" /
                         "esp32" / "tools" / "esptool_py").glob("*/esptool"))
    if not candidates:
        raise RuntimeError("esptool is unavailable")
    return candidates[-1]


def run_evidence_command(command: list[str], output: Path, label: str,
                         timeout: float = 30.0) -> None:
    completed = subprocess.run(
        command, cwd=PROJECT, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout, check=False)
    (output / f"{label}.log").write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        raise RuntimeError(
            f"diagnostic command {label} failed with exit {completed.returncode}")


def diagnose_rom(args: argparse.Namespace, output: Path) -> dict[str, object]:
    if args.authority is None:
        raise RuntimeError("ROM diagnosis requires --authority")
    esptool = find_esptool()
    common = [str(esptool), "--chip", "esp32s3", "--port", args.rom_port,
              "--baud", "115200", "--no-stub"]
    run_evidence_command(common + ["chip-id"], output, "rom-chip-id")
    run_evidence_command(common + ["flash-id"], output, "rom-flash-id")
    verdict = output / "rom-identity-verdict.json"
    run_evidence_command([
        sys.executable, str(PROJECT / "tools" / "validate-flash-identity.py"),
        "evidence", "--authority", str(args.authority),
        "--chip-log", str(output / "rom-chip-id.log"),
        "--flash-log", str(output / "rom-flash-id.log"),
        "--output", str(verdict),
    ], output, "rom-identity-validation")
    regions = (
        ("partition-table", "0x8000", "0x1000"),
        ("nvs.private", "0x9000", "0x5000"),
        ("coredump", "0xff0000", "0x10000"),
    )
    evidence: dict[str, object] = {
        "status": "captured", "port": args.rom_port,
        "identityVerdict": verdict.name, "regions": {},
    }
    for name, offset, size in regions:
        target = output / f"{name}.bin"
        run_evidence_command(
            common + ["read-flash", offset, size, str(target)],
            output, f"rom-read-{name}", 60.0)
        if name == "nvs.private":
            target.chmod(0o600)
        evidence["regions"][name] = {
            "file": target.name, "sizeBytes": target.stat().st_size,
            "sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
        }
    if args.elf is not None and args.elf.is_file():
        core_tool_candidates = sorted(
            (Path.home() / ".espressif" / "python_env").glob(
                "idf*_py*_env/bin/esp-coredump"))
        if core_tool_candidates:
            try:
                run_evidence_command([
                    str(core_tool_candidates[-1]), "--chip", "esp32s3",
                    "info_corefile", "--core", str(output / "coredump.bin"),
                    "--core-format", "raw", str(args.elf),
                ], output, "coredump-report", 60.0)
                evidence["coredumpReport"] = {
                    "status": "decoded", "file": "coredump-report.log"}
            except RuntimeError as error:
                # A ROM snapshot can predate the candidate ELF supplied by the
                # caller. Preserve the raw dump and decoder output so a matching
                # historical ELF can be applied later; evidence collection must
                # still finish and write its manifest.
                evidence["coredumpReport"] = {
                    "status": "decode_failed",
                    "file": "coredump-report.log",
                    "reason": str(error),
                }
        else:
            evidence["coredumpReport"] = {"status": "decoder_unavailable"}
    else:
        evidence["coredumpReport"] = {"status": "elf_unavailable"}
    return evidence


def diagnose(args: argparse.Namespace) -> int:
    output = run_dir("diagnose")
    result: dict[str, object] = {
        "schema": "pokepod.fixture-diagnosis.v1",
        "capturedAt": datetime.now(timezone.utc).isoformat(),
        "macVoice": copy_mac_voice_diagnostic(output),
    }
    app_errors: dict[str, str] = {}
    candidates = [args.port] if args.port else matching_ports(args.port_pattern)
    for port in candidates:
        if port is None:
            continue
        try:
            result["mode"] = "application"
            result["application"] = collect_into(output, port, args.timeout)
            result["port"] = port
            write_json(output / "evidence.json", result)
            print(output)
            return 0
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            app_errors[port] = str(error)
    result["applicationErrors"] = app_errors
    if args.rom_port:
        result["mode"] = "rom"
        result["rom"] = diagnose_rom(args, output)
        write_json(output / "evidence.json", result)
        print(output)
        return 0
    result["mode"] = "unavailable"
    write_json(output / "evidence.json", result)
    print(output)
    return 3


def latest_boot(runtime: dict[str, object]) -> dict[str, int]:
    records = runtime.get("records")
    if not isinstance(records, list):
        raise RuntimeError("runtime diagnostics have no records")
    for record in records:
        if (isinstance(record, dict) and record.get("subsystem") == "boot"
                and record.get("stage") == "boot"):
            sequence = record.get("sequence")
            reset_reason = record.get("reset_reason")
            if isinstance(sequence, int) and isinstance(reset_reason, int):
                return {"sequence": sequence, "resetReason": reset_reason}
    raise RuntimeError("runtime diagnostics have no boot record")


def require_same_boot(before: dict[str, int], after: dict[str, int],
                      phase: str) -> None:
    if after["sequence"] != before["sequence"]:
        raise RuntimeError(
            f"unexpected device restart during {phase}: "
            f"before={before} after={after}"
        )


def update(port: str, firmware: Path, timeout: float, port_pattern: str,
           app_timeout: float) -> int:
    if not firmware.is_file():
        raise RuntimeError(f"firmware image does not exist: {firmware}")
    image = firmware.read_bytes()
    if not 1024 <= len(image) <= 0x300000:
        raise RuntimeError("firmware image must be between 1 KiB and 3 MiB")
    output = run_dir("update")
    hello = cdc(port, "hello", output, min(timeout, 5.0))
    before = link_identity(port, output, min(timeout, 5.0))
    expected_device_id = str(before["deviceId"])
    expected_build = expected_build_from_artifact(firmware)
    expected_identity = load_firmware_artifact_identity(firmware)
    for key in ("sourceRevision", "firmwareVersion", "appElfSha256"):
        if expected_build[key] != expected_identity[key]:
            raise RuntimeError(f"artifact identity disagreement for {key}")
    metadata = {
        "port": port, "deviceId": expected_device_id, "hello": hello,
        "identity": before, "firmware": str(firmware), "bytes": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "expectedBuild": expected_build,
        "expectedIdentity": expected_identity,
    }
    write_json(output / "request.json", metadata)
    command = [sys.executable, str(CDC), port, "--firmware", str(firmware),
               "--source-revision", expected_identity["sourceRevision"],
               "--firmware-version", expected_identity["firmwareVersion"],
               "--app-elf-sha256", expected_identity["appElfSha256"],
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
    require_expected_build(post_identity, expected_build)
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
        stderr=subprocess.PIPE, timeout=1800.0, check=False,
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
    hello = cdc(args.port, "hello", output, args.timeout)
    identity = link_identity(args.port, output, args.timeout)
    expected_device_id = str(identity["deviceId"])
    pre_runtime = cdc(args.port, "get-runtime-diagnostics", output,
                      args.timeout)
    pre_boot = latest_boot(pre_runtime)
    write_json(output / "pre.json", {
        "hello": hello, "identity": identity,
        "status": cdc(args.port, "status", output, args.timeout),
        "runtime": pre_runtime, "boot": pre_boot,
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
            mid_identity = link_identity(args.port, output, args.timeout)
            mid_runtime = cdc(args.port, "get-runtime-diagnostics", output,
                              args.timeout)
            mid_boot = latest_boot(mid_runtime)
            require_same_boot(pre_boot, mid_boot, "active provisioning")
            write_json(output / "active.json", {
                "identity": mid_identity,
                "status": cdc(args.port, "status", output, args.timeout),
                "runtime": mid_runtime,
                "boot": mid_boot,
            })
            cdc(args.port, "provisioning-stop", output, args.timeout)
        else:
            raise RuntimeError(f"unsupported scenario {args.scenario}")
        time.sleep(1.0)
        app_port, post_identity = wait_for_application(
            args.port_pattern, expected_device_id, output,
            args.app_timeout, args.timeout,
        )
        post_runtime = cdc(app_port, "get-runtime-diagnostics", output,
                           args.timeout)
        post_boot = latest_boot(post_runtime)
        if args.scenario == "recording":
            require_same_boot(pre_boot, post_boot, "recording")
        elif (post_boot["sequence"] != pre_boot["sequence"]
              and post_boot["resetReason"] != 3):
            raise RuntimeError(
                "provisioning stop produced an unexpected reset: "
                f"before={pre_boot} after={post_boot}"
            )
        write_json(output / "post.json", {
            "port": app_port,
            "identity": post_identity,
            "status": cdc(app_port, "status", output, args.timeout),
            "runtime": post_runtime,
            "boot": post_boot,
        })
    except RuntimeError as error:
        failure = error
    if failure is None:
        write_json(output / "result.json", {
            "scenario": args.scenario, "passed": True,
            "automaticRecovery": False,
            "preBoot": pre_boot,
            "postBoot": post_boot,
            "plannedStopRestart": (
                args.scenario == "provisioning"
                and post_boot["sequence"] != pre_boot["sequence"]
            ),
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
    diagnose_parser = sub.add_parser("diagnose")
    diagnose_parser.add_argument("--port")
    diagnose_parser.add_argument("--port-pattern", default="/dev/cu.usbmodem*")
    diagnose_parser.add_argument("--rom-port")
    diagnose_parser.add_argument("--authority", type=Path)
    diagnose_parser.add_argument("--elf", type=Path)
    diagnose_parser.add_argument("--timeout", type=float, default=3.0)
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
            write_json(output / "evidence.json", {
                "hello": cdc(args.port, "hello", output, args.timeout),
                "identity": link_identity(args.port, output, args.timeout),
                "status": cdc(args.port, "status", output, args.timeout),
            })
            print(output)
            return 0
        if args.operation == "collect":
            return collect(args.port, "collect", args.timeout)
        if args.operation == "diagnose":
            return diagnose(args)
        if args.operation == "update":
            return update(args.port, args.firmware, args.timeout,
                          args.port_pattern, args.app_timeout)
        if args.operation == "doctor":
            output = run_dir("doctor")
            profile = FixtureControlProfile.load(args.control_profile)
            evidence = profile.capabilities()
            write_json(output / "control-capabilities.json", evidence)
            if profile.backend != "command":
                write_json(output / "evidence.json", evidence)
                print(output)
                return 4
            controller = FixtureController(profile, output)
            controller.doctor()
            evidence["liveController"] = "passed"
            write_json(output / "evidence.json", evidence)
            print(output)
            return 0
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
