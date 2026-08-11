#!/usr/bin/env python3
"""Cross-platform hashing and file metadata helpers for build scripts."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def argv_sha256(arguments: list[str]) -> str:
    digest = hashlib.sha256()
    for argument in arguments:
        encoded = argument.encode("utf-8", "surrogateescape")
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    sha = subparsers.add_parser("sha256")
    sha.add_argument("paths", nargs="+")
    value = subparsers.add_parser("sha256-value")
    value.add_argument("path")
    size = subparsers.add_parser("size")
    size.add_argument("path")
    argv = subparsers.add_parser("argv-sha256")
    argv.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    if args.command == "sha256":
        for raw in args.paths:
            path = Path(raw)
            print(f"{file_sha256(path)}  {path}")
    elif args.command == "sha256-value":
        print(file_sha256(Path(args.path)))
    elif args.command == "size":
        print(Path(args.path).stat().st_size)
    else:
        print(argv_sha256(args.arguments))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
