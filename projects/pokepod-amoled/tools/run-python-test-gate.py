#!/usr/bin/env python3
"""Run one explicit Python host-test gate and reject unclassified tests."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
from pathlib import Path
import subprocess
import sys


TOOLS = Path(__file__).resolve().parent
MANIFEST = TOOLS / "test-gates.json"
VALID_GATES = ("source", "asset", "toolchain", "repository")


def load_manifest() -> dict[str, object]:
    payload = json.loads(MANIFEST.read_text(encoding="utf-8"))
    assert payload.get("schema") == "pokepod.host-test-gates.v1"
    return payload


def validate(payload: dict[str, object]) -> None:
    assigned: dict[str, str] = {}
    for gate in VALID_GATES:
        names = payload.get(gate)
        assert isinstance(names, list), f"missing test gate: {gate}"
        for name in names:
            assert isinstance(name, str) and name.startswith("test-") and name.endswith(".py")
            assert name not in assigned, f"test appears in two gates: {name}"
            assigned[name] = gate
            assert (TOOLS / name).is_file(), f"classified test is missing: {name}"
    discovered = {path.name for path in TOOLS.glob("test-*.py")}
    classified = set(assigned)
    assert discovered == classified, (
        f"test gate classification mismatch; unclassified={sorted(discovered-classified)}, "
        f"missing={sorted(classified-discovered)}"
    )


def execute(name: str) -> tuple[str, int, str]:
    path = TOOLS / name
    completed = subprocess.run(
        [sys.executable, str(path)],
        cwd=TOOLS.parent,
        text=True,
        capture_output=True,
    )
    return name, completed.returncode, completed.stdout + completed.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("gate", choices=VALID_GATES)
    parser.add_argument("--jobs", type=int)
    args = parser.parse_args()
    payload = load_manifest()
    validate(payload)
    names = payload[args.gate]
    assert isinstance(names, list)
    default_jobs = min(4, os.cpu_count() or 1) if args.gate == "source" else 1
    jobs = default_jobs if args.jobs is None else args.jobs
    if jobs < 1:
        raise SystemExit("--jobs must be at least 1")

    results: dict[str, tuple[int, str]] = {}
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = {executor.submit(execute, str(name)): str(name) for name in names}
        for future in as_completed(futures):
            name, returncode, output = future.result()
            results[name] = (returncode, output)

    failed = False
    for name in names:
        returncode, output = results[str(name)]
        if output:
            print(output, end="" if output.endswith("\n") else "\n")
        if returncode != 0:
            print(f"FAIL {name} ({returncode})", file=sys.stderr)
            failed = True
    if failed:
        return 1
    print(f"PASS python_test_gate_{args.gate} ({len(names)} tests, jobs={jobs})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
