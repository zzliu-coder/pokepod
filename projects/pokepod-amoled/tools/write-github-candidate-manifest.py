#!/usr/bin/env python3
"""Write a small exact-SHA manifest for the files uploaded by GitHub CI."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--file", dest="files", action="append", type=Path, required=True)
    args = parser.parse_args()
    records = []
    for path in args.files:
        payload = path.read_bytes()
        records.append({
            "file": path.name,
            "bytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
        })
    args.output.write_text(json.dumps({
        "schema": "pokepod.github-fast-candidate.v1",
        "sourceRevision": args.source_revision,
        "sourceClean": True,
        "status": "verified",
        "files": records,
    }, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
