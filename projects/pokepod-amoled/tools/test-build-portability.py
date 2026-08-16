#!/usr/bin/env python3
"""Portable, home-independent contracts for the PokePod build environment."""

from __future__ import annotations

import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

from pokepod_build_env import (  # noqa: E402
    BuildEnvironmentError,
    PRODUCTION_CORE_VERSION,
    resolve_build_environment,
)


def fake_cli(path: Path, data: Path, user: Path, version: str) -> None:
    path.write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        "case \"${1-}:${2-}:${3-}\" in\n"
        f"  config:get:directories.data) printf '%s\\n' '{data}' ;;\n"
        f"  config:get:directories.user) printf '%s\\n' '{user}' ;;\n"
        "  core:list:) printf 'ID Installed Latest Name\\n' ; "
        f"printf 'esp32:esp32 {version} {version} esp32\\n' ;;\n"
        "  version::) printf 'arduino-cli Version: test\\n' ;;\n"
        "  *) printf 'unexpected fake cli arguments: %s\\n' \"$*\" >&2; exit 9 ;;\n"
        "esac\n",
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def create_layout(root: Path, version: str) -> tuple[Path, Path, Path]:
    data = root / "portable data"
    user = root / "portable user"
    platform = data / "packages/esp32/hardware/esp32" / version
    sdk = data / "packages/esp32/tools/esp32s3-libs" / version
    gfx = user / "libraries/GFX_Library_for_Arduino"
    platform.mkdir(parents=True)
    sdk.mkdir(parents=True)
    gfx.mkdir(parents=True)
    return data, user, gfx


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    data, user, gfx = create_layout(root, PRODUCTION_CORE_VERSION)
    cli = root / "tools with spaces" / "arduino-cli"
    cli.parent.mkdir()
    fake_cli(cli, data, user, PRODUCTION_CORE_VERSION)

    environment = {
        "ARDUINO_CLI": str(cli),
        "HOME": str(root / "home-must-not-be-used"),
    }
    resolved = resolve_build_environment(environment)
    assert resolved.arduino_cli == str(cli.resolve())
    assert resolved.arduino_data_dir == str(data.resolve())
    assert resolved.arduino_user_dir == str(user.resolve())
    assert resolved.gfx_library == str(gfx.resolve())
    assert resolved.esp32_core_version == PRODUCTION_CORE_VERSION
    assert resolved.core_profile == "production"
    shell = resolved.shell_assignments()
    assert "portable data" in shell and "'" in shell
    assert "export ARDUINO_DIRECTORIES_DATA=" in shell
    assert "export ARDUINO_DIRECTORIES_USER=" in shell

    command_environment = os.environ.copy()
    command_environment.update(environment)
    json_output = subprocess.check_output(
        ["python3", str(TOOLS / "pokepod_build_env.py"), "--format", "json"],
        env=command_environment,
        text=True,
    )
    assert json.loads(json_output)["esp32_core_version"] == PRODUCTION_CORE_VERSION

    matrix_version = "3.3.11"
    matrix_data, matrix_user, _ = create_layout(root / "matrix", matrix_version)
    matrix_cli = root / "matrix-cli"
    fake_cli(matrix_cli, matrix_data, matrix_user, matrix_version)
    matrix_base = {"ARDUINO_CLI": str(matrix_cli)}
    try:
        resolve_build_environment(matrix_base)
        raise AssertionError("non-production core passed the production gate")
    except BuildEnvironmentError as exc:
        assert "required" in str(exc)
    matrix_environment = {
        **matrix_base,
        "POKEPOD_CORE_MATRIX": "1",
        "POKEPOD_ESP32_CORE_VERSION": matrix_version,
    }
    matrix = resolve_build_environment(matrix_environment)
    assert matrix.esp32_core_version == matrix_version
    assert matrix.core_profile == "matrix"

    payload = root / "payload.bin"
    payload.write_bytes(b"portable-pokepod")
    portable = TOOLS / "portable_build_utils.py"
    digest = subprocess.check_output(
        ["python3", str(portable), "sha256-value", str(payload)], text=True
    ).strip()
    assert len(digest) == 64
    size = subprocess.check_output(
        ["python3", str(portable), "size", str(payload)], text=True
    ).strip()
    assert size == str(len(b"portable-pokepod"))

    artifact = root / "artifact.json"
    flash_policy = root / "flash-resource.json"
    flash_policy.write_text(
        json.dumps(
            {
                "schema": "pokepod.flash-size-policy.v1",
                "programBytes": 16,
                "slotBytes": 0x300000,
                "remainingBytes": 0x300000 - 16,
                "percent": 0.0005,
                "tier": "green",
                "releaseAllowed": True,
                "requiredAction": "keep implementation simple",
                "thresholds": {},
            }
        ),
        encoding="utf-8",
    )
    version_header = ROOT / "firmware/PokePodAmoled/FirmwareVersion.h"
    subprocess.check_call(
        [
            "python3", str(TOOLS / "write-artifact-manifest.py"),
            "--output", str(artifact), "--lane", "fast",
            "--source-revision", "abc", "--source-dirty", "false",
            "--build-input", "1" * 64, "--binary-sha256", "2" * 64,
            "--binary-size", "16", "--created-at", "2026-08-11T00:00:00Z",
            "--flash-policy", str(flash_policy),
            "--resource-review-approved", "false",
            "--fqbn", "esp32:esp32:esp32s3:test", "--core-version", "3.3.8",
            "--core-profile", "production", "--app-offset", "0x10000",
            "--vendor-revision", "def",
            "--firmware-version-header", str(version_header),
        ]
    )
    manifest = json.loads(artifact.read_text())
    assert manifest["binary"]["flashOffset"] == "0x10000"
    assert manifest["binary"]["slotSizeBytes"] == 0x300000
    assert manifest["binary"]["resourceTier"] == "green"
    assert manifest["resourceReview"]["required"] is False
    assert manifest["resourceReview"]["approved"] is False
    assert manifest["toolchain"]["coreProfile"] == "production"
    assert manifest["firmwareVersion"] == "2.0.0"

build = (ROOT / "firmware/build.sh").read_text(encoding="utf-8")
ble_service = (ROOT / "firmware/PokePodAmoled/BleVoiceService.cpp").read_text(
    encoding="utf-8"
)
verify = (ROOT / "verify.sh").read_text(encoding="utf-8")
usb_contract = (TOOLS / "test-usb-connection-contract.py").read_text(
    encoding="utf-8"
)
combined = build + usb_contract
forbidden_home = "/" + "Users/" + "zheliu"
forbidden_app_root = "/" + "Applications"
assert forbidden_home not in combined
assert forbidden_app_root not in combined
assert "Release build requires a clean PokePod tree" in build
assert "Release build requires a Git commit" in build
assert "Release build requires a positive Git commit timestamp" in build
assert 'git -C "$PROJECT_DIR" show -s --format=%ct "$SOURCE_REVISION"' in build
assert "Release build requires the production ESP32 core profile" in build
assert 'source_revision=${SOURCE_REVISION:-unknown}' in build
assert "stat -f" not in build and "shasum" not in build
assert "shasum" not in verify and "portable_build_utils.py" in verify
assert "PRODUCTION_CORE_VERSION = \"3.3.8\"" in (
    TOOLS / "pokepod_build_env.py"
).read_text()
assert "POKEPOD_CORE_MATRIX" in build
assert "PartitionScheme=app3M_fat9M_16MB" in build
assert "CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1" in build
assert "CONFIG_BT_CTRL_BLE_LLCP_CONN_UPDATE=y" in build
assert "#define CONFIG_BT_CTRL_BLE_LLCP_CONN_UPDATE 1" in build
assert "APP_ONLY_FLASH_OFFSET=0x10000" in build
assert 'FIRMWARE_VERSION_HEADER="$SKETCH_DIR/FirmwareVersion.h"' in build
assert "--firmware-version-header" in build
assert "kFirmwareVersion" in ble_service
assert '\\"firmwareVersion\\":\\"2.0.0' not in ble_service

print("PASS test_build_portability (fake home, production/matrix, portable metadata)")
