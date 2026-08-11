#!/usr/bin/env python3
"""Resolve the host-side Arduino inputs used by the PokePod build.

The resolver is deliberately independent from a developer's home directory.
Explicit environment variables win, then arduino-cli's own configuration is
used.  macOS may additionally discover the CLI bundled with Arduino IDE via
Spotlight, without assuming a fixed system application directory.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
from typing import Mapping


PRODUCTION_CORE_VERSION = "3.3.8"


class BuildEnvironmentError(RuntimeError):
    pass


@dataclass(frozen=True)
class BuildEnvironment:
    arduino_cli: str
    arduino_data_dir: str
    arduino_user_dir: str
    gfx_library: str
    esp32_core_version: str
    esp32_platform_dir: str
    esp32_s3_sdk_dir: str
    core_profile: str

    def shell_assignments(self) -> str:
        names = {
            "ARDUINO_CLI": self.arduino_cli,
            "ARDUINO_DATA_DIR": self.arduino_data_dir,
            "ARDUINO_USER_DIR": self.arduino_user_dir,
            "ARDUINO_DIRECTORIES_DATA": self.arduino_data_dir,
            "ARDUINO_DIRECTORIES_USER": self.arduino_user_dir,
            "GFX_LIBRARY": self.gfx_library,
            "ESP32_CORE_VERSION": self.esp32_core_version,
            "ESP32_PLATFORM_DIR": self.esp32_platform_dir,
            "ESP32_S3_SDK_DIR": self.esp32_s3_sdk_dir,
            "POKEPOD_CORE_PROFILE": self.core_profile,
        }
        return "\n".join(
            f"export {name}={shlex.quote(value)}" for name, value in names.items()
        )


def _resolve_executable(value: str) -> Path | None:
    candidate = Path(value).expanduser()
    if candidate.parent != Path(".") or "/" in value:
        return candidate.resolve() if candidate.is_file() else None
    located = shutil.which(value)
    return Path(located).resolve() if located else None


def _spotlight_arduino_cli() -> Path | None:
    if platform.system() != "Darwin":
        return None
    mdfind = shutil.which("mdfind")
    if not mdfind:
        return None
    try:
        result = subprocess.run(
            [mdfind, "kMDItemCFBundleIdentifier == 'cc.arduino.IDE2'"],
            check=True,
            text=True,
            capture_output=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    relative = Path("Contents/Resources/app/lib/backend/resources/arduino-cli")
    for line in result.stdout.splitlines():
        candidate = Path(line.strip()) / relative
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    return None


def find_arduino_cli(env: Mapping[str, str]) -> Path:
    explicit = env.get("ARDUINO_CLI", "").strip()
    if explicit:
        resolved = _resolve_executable(explicit)
        if resolved and os.access(resolved, os.X_OK):
            return resolved
        raise BuildEnvironmentError(f"ARDUINO_CLI is not executable: {explicit}")
    on_path = _resolve_executable("arduino-cli")
    if on_path and os.access(on_path, os.X_OK):
        return on_path
    bundled = _spotlight_arduino_cli()
    if bundled:
        return bundled
    raise BuildEnvironmentError(
        "Arduino CLI was not found; install arduino-cli or set ARDUINO_CLI"
    )


def _run(
    cli: Path, *arguments: str, environment: Mapping[str, str] | None = None
) -> str:
    try:
        completed = subprocess.run(
            [str(cli), *arguments],
            check=True,
            text=True,
            capture_output=True,
            env=None if environment is None else dict(environment),
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        stderr = getattr(exc, "stderr", "") or ""
        detail = stderr.strip() or str(exc)
        raise BuildEnvironmentError(
            f"arduino-cli {' '.join(arguments)} failed: {detail}"
        ) from exc
    return completed.stdout.strip()


def _configured_directory(
    env: Mapping[str, str], env_name: str, config_name: str, cli: Path
) -> Path:
    explicit = env.get(env_name, "").strip()
    value = explicit or _run(cli, "config", "get", config_name)
    if not value:
        raise BuildEnvironmentError(
            f"Arduino {config_name} is empty; set {env_name} explicitly"
        )
    return Path(value).expanduser().resolve()


def _installed_esp32_version(
    cli: Path, environment: Mapping[str, str]
) -> str:
    for line in _run(cli, "core", "list", environment=environment).splitlines():
        fields = line.split()
        if fields and fields[0] == "esp32:esp32" and len(fields) >= 2:
            return fields[1]
    raise BuildEnvironmentError("ESP32 Arduino core is not installed")


def resolve_build_environment(
    env: Mapping[str, str] | None = None,
) -> BuildEnvironment:
    values = os.environ if env is None else env
    cli = find_arduino_cli(values)
    data_dir = _configured_directory(
        values, "ARDUINO_DATA_DIR", "directories.data", cli
    )
    user_dir = _configured_directory(
        values, "ARDUINO_USER_DIR", "directories.user", cli
    )
    cli_environment = dict(os.environ)
    cli_environment.update(values)
    cli_environment["ARDUINO_DIRECTORIES_DATA"] = str(data_dir)
    cli_environment["ARDUINO_DIRECTORIES_USER"] = str(user_dir)
    installed = _installed_esp32_version(cli, cli_environment)

    matrix_enabled = values.get("POKEPOD_CORE_MATRIX", "") == "1"
    requested = values.get("POKEPOD_ESP32_CORE_VERSION", "").strip()
    if matrix_enabled:
        if not requested:
            raise BuildEnvironmentError(
                "POKEPOD_CORE_MATRIX=1 requires POKEPOD_ESP32_CORE_VERSION"
            )
        profile = "matrix"
    else:
        if requested and requested != PRODUCTION_CORE_VERSION:
            raise BuildEnvironmentError(
                "Non-production cores require POKEPOD_CORE_MATRIX=1"
            )
        requested = PRODUCTION_CORE_VERSION
        profile = "production"

    if installed != requested:
        raise BuildEnvironmentError(
            f"ESP32 core {requested} is required, but arduino-cli selects {installed}"
        )

    gfx_explicit = values.get("GFX_LIBRARY", "").strip()
    gfx = (
        Path(gfx_explicit).expanduser().resolve()
        if gfx_explicit
        else (user_dir / "libraries" / "GFX_Library_for_Arduino").resolve()
    )
    platform_dir = (
        data_dir / "packages" / "esp32" / "hardware" / "esp32" / requested
    ).resolve()
    sdk_dir = (
        data_dir / "packages" / "esp32" / "tools" / "esp32s3-libs" / requested
    ).resolve()
    return BuildEnvironment(
        arduino_cli=str(cli),
        arduino_data_dir=str(data_dir),
        arduino_user_dir=str(user_dir),
        gfx_library=str(gfx),
        esp32_core_version=requested,
        esp32_platform_dir=str(platform_dir),
        esp32_s3_sdk_dir=str(sdk_dir),
        core_profile=profile,
    )


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("--format", choices=("json", "shell"), default="json")
    args = parser.parse_args()
    try:
        resolved = resolve_build_environment()
    except BuildEnvironmentError as exc:
        raise SystemExit(f"PokePod build environment error: {exc}") from exc
    if args.format == "shell":
        print(resolved.shell_assignments())
    else:
        print(json.dumps(asdict(resolved), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
