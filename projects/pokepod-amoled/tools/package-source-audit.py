#!/usr/bin/env python3
"""Create a deterministic, source-only PokePod audit archive from a clean HEAD."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import zipfile


PROJECT_PATH = PurePosixPath("projects/pokepod-amoled")
EXCLUDED_PARTS = {
    ".cache",
    ".codeprinter",
    ".pio",
    "_audit",
    "__pycache__",
    "build",
    "output",
    "work",
}
EXCLUDED_SUFFIXES = {
    ".a",
    ".a4",
    ".bin",
    ".bmp",
    ".dylib",
    ".elf",
    ".gif",
    ".gz",
    ".heic",
    ".jpeg",
    ".jpg",
    ".map",
    ".mp3",
    ".mp4",
    ".o",
    ".pdf",
    ".png",
    ".tar",
    ".wav",
    ".webp",
    ".zip",
}
SENSITIVE_SUFFIXES = {
    ".db",
    ".der",
    ".key",
    ".jks",
    ".kdbx",
    ".keystore",
    ".mobileprovision",
    ".p12",
    ".p8",
    ".pem",
    ".pfx",
    ".sqlite",
    ".sqlite3",
}
SENSITIVE_FILENAMES = {
    ".env",
    ".env.local",
    ".envrc",
    ".netrc",
    ".npmrc",
    "credentials.json",
    "credentials.yaml",
    "credentials.yml",
    "id_ed25519",
    "id_rsa",
    "secrets.json",
    "secrets.yaml",
    "secrets.yml",
}
SENSITIVE_CONTENT = {
    "private-key": re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "tencent-secret-id": re.compile(rb"\bAKID[A-Za-z0-9]{12,}\b"),
}


def run_git(repo: Path, *args: str, text: bool = True) -> str | bytes:
    result = subprocess.run(
        ["git", *args],
        cwd=repo,
        check=False,
        capture_output=True,
        text=text,
    )
    if result.returncode != 0:
        stderr = result.stderr if text else result.stderr.decode("utf-8", "replace")
        raise RuntimeError(stderr.strip() or f"git {' '.join(args)} failed")
    return result.stdout


def source_path(path: str) -> bool:
    pure = PurePosixPath(path)
    if not pure.is_relative_to(PROJECT_PATH):
        return False
    relative = pure.relative_to(PROJECT_PATH)
    if any(part in EXCLUDED_PARTS for part in relative.parts):
        return False
    return relative.suffix.lower() not in EXCLUDED_SUFFIXES


def sensitive_path(path: str) -> bool:
    pure = PurePosixPath(path)
    name = pure.name.lower()
    return (
        name in SENSITIVE_FILENAMES
        or name.startswith(".env.")
        or name.startswith("credentials.")
        or name.startswith("secrets.")
        or pure.suffix.lower() in SENSITIVE_SUFFIXES
    )


def tracked_paths(repo: Path, commit: str) -> list[str]:
    output = run_git(
        repo,
        "ls-tree",
        "-r",
        "--name-only",
        commit,
        "--",
        str(PROJECT_PATH),
    )
    assert isinstance(output, str)
    return sorted(output.splitlines())


def blob(repo: Path, commit: str, path: str) -> bytes:
    output = run_git(repo, "show", f"{commit}:{path}", text=False)
    assert isinstance(output, bytes)
    return output


def zip_info(name: str, mode: int = 0o644) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (mode & 0xFFFF) << 16
    info.create_system = 3
    return info


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True, help="Git worktree root")
    parser.add_argument("--output", type=Path, required=True, help="Destination .zip")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo = args.repo.resolve()
    output = args.output.resolve()

    head = str(run_git(repo, "rev-parse", "HEAD")).strip()
    status = str(
        run_git(
            repo,
            "status",
            "--porcelain=v1",
            "--untracked-files=all",
            "--",
            str(PROJECT_PATH),
        )
    ).strip()
    if status:
        print("source audit package requires a clean PokePod tree", file=sys.stderr)
        print(status, file=sys.stderr)
        return 2

    tracked = tracked_paths(repo, head)
    sensitive_paths = [path for path in tracked if sensitive_path(path)]
    if sensitive_paths:
        print("tracked sensitive files refuse source audit packaging", file=sys.stderr)
        for path in sensitive_paths:
            print(path, file=sys.stderr)
        return 6
    paths = [path for path in tracked if source_path(path)]
    if not paths:
        print("no PokePod source files found", file=sys.stderr)
        return 3

    short = head[:12]
    prefix = f"PokePod-source-audit-{short}"
    records: list[dict[str, object]] = []
    payloads: list[tuple[str, bytes, int]] = []
    for path in paths:
        data = blob(repo, head, path)
        content_hits = [name for name, pattern in SENSITIVE_CONTENT.items() if pattern.search(data)]
        if content_hits:
            print(f"sensitive content refuses packaging: {path}: {','.join(content_hits)}", file=sys.stderr)
            return 6
        archive_path = str(PurePosixPath(prefix) / PurePosixPath(path).relative_to(PROJECT_PATH))
        executable = path.endswith((".sh", ".py")) and data.startswith(b"#!")
        mode = 0o755 if executable else 0o644
        payloads.append((archive_path, data, mode))
        records.append(
            {
                "path": str(PurePosixPath(path).relative_to(PROJECT_PATH)),
                "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        )

    manifest = {
        "schema": "pokepod.source-audit.v1",
        "commit": head,
        "project": str(PROJECT_PATH),
        "sourceOnly": True,
        "fileCount": len(records),
        "files": records,
        "excluded": {
            "directories": sorted(EXCLUDED_PARTS),
            "binarySuffixes": sorted(EXCLUDED_SUFFIXES),
        },
        "sensitiveScan": {
            "status": "pass",
            "pathSuffixes": sorted(SENSITIVE_SUFFIXES),
            "filenames": sorted(SENSITIVE_FILENAMES),
            "contentRules": sorted(SENSITIVE_CONTENT),
        },
    }
    manifest_bytes = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode()
    checksums = "".join(
        f"{record['sha256']}  {record['path']}\n" for record in records
    ).encode()
    guide = f"""# PokePod source audit package

Exact commit: `{head}`

This archive contains tracked PokePod source, tests, build scripts, configuration and text documentation from one clean Git commit. Firmware binaries, build products, caches, device backups, logs, recordings and other binary assets are excluded.

Review priorities:

1. User-data atomicity and power-loss recovery.
2. SD ownership, bounded I/O slices and File cleanup under the matching lease.
3. USB/Wi-Fi session cancellation, the single absolute five-minute Wi-Fi deadline and durable result replay.
4. Recorder capture/storage task ownership, queue overflow truth and shutdown quiescence.
5. Power blockers, 1-minute light sleep, 3-minute deep sleep and wake-source truth.
6. Fixed-capacity/PSRAM budgets, malformed input, path traversal and recovery journal validation.

Frozen cross-end contracts:

- PokePod Link v2 wire/schema, BLE Voice v1 UUID/frame layout and permanent device identity.
- PokeCapsule processing v1/v2 compatibility and unknown-schema read-only behavior.
- The Wi-Fi sync window uses one absolute five-minute deadline; activity never rearms it.
- Power policy targets one-minute light sleep and three-minute deep sleep when real blockers are absent.
- Mac, Android and shared protocol repositories are outside this package. Recommend a cross-end change only when the device source proves the current contract cannot be preserved.

For every finding, report severity (P0/P1/P2/P3), exact source path, reachable failure sequence, user/data impact, smallest architectural fix and an automated test that would close it. Separate source-proven defects from hardware-only hypotheses.

Host tests and builds are evidence inputs only. This package does not claim Release reproducibility, device flashing, current draw, RF behavior or real-SD power-cut acceptance.
""".encode()

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, data, mode in payloads:
                archive.writestr(zip_info(name, mode), data)
            archive.writestr(zip_info(f"{prefix}/_audit/AUDIT_GUIDE.md"), guide)
            archive.writestr(zip_info(f"{prefix}/_audit/SOURCE_MANIFEST.json"), manifest_bytes)
            archive.writestr(zip_info(f"{prefix}/_audit/SOURCE_MANIFEST.sha256"), checksums)
        os.replace(temporary, output)
    finally:
        if temporary.exists():
            temporary.unlink()

    print(
        json.dumps(
            {
                "output": str(output),
                "commit": head,
                "fileCount": len(records),
                "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
            },
            ensure_ascii=False,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
