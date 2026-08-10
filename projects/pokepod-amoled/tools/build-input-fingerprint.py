#!/usr/bin/env python3
"""Create a deterministic SHA-256 for inputs that affect a PokePod build."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".ino", ".s", ".S",
    ".properties", ".txt", ".py", ".sh",
}


def add_value(digest: "hashlib._Hash", kind: str, label: str, data: bytes) -> None:
    for value in (kind.encode(), label.encode(), data):
        digest.update(len(value).to_bytes(8, "big"))
        digest.update(value)


def source_files(root: Path) -> list[Path]:
    return sorted(
        (path for path in root.rglob("*")
         if path.is_file() and path.suffix in SOURCE_SUFFIXES),
        key=lambda path: path.relative_to(root).as_posix(),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--file", action="append", default=[])
    parser.add_argument("--tree", action="append", default=[])
    parser.add_argument("--literal", action="append", default=[])
    args = parser.parse_args()

    digest = hashlib.sha256()
    for literal in args.literal:
        add_value(digest, "literal", literal, literal.encode())

    for raw in args.file:
        path = Path(raw).resolve(strict=True)
        if not path.is_file():
            raise SystemExit(f"build fingerprint input is not a file: {path}")
        add_value(digest, "file", str(path), path.read_bytes())

    for raw in args.tree:
        root = Path(raw).resolve(strict=True)
        if not root.is_dir():
            raise SystemExit(f"build fingerprint input is not a directory: {root}")
        files = source_files(root)
        add_value(digest, "tree", str(root), str(len(files)).encode())
        for path in files:
            add_value(
                digest,
                "tree-file",
                path.relative_to(root).as_posix(),
                path.read_bytes(),
            )

    print(digest.hexdigest())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
