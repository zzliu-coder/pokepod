#!/usr/bin/env python3
"""Compile and execute PokePod's source-only C++ host tests in parallel."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


PROJECT = Path(__file__).resolve().parents[1]
FIRMWARE = PROJECT / "firmware"
TESTS = FIRMWARE / "tests"
SOURCE = FIRMWARE / "PokePodAmoled"
SUPPORT = TESTS / "support"


def execute(test: Path, output: Path, compiler: list[str]) -> tuple[str, str]:
    name = test.stem
    binary = output / name
    compile_result = subprocess.run(
        [
            *compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{SUPPORT}",
            f"-I{SOURCE}",
            str(test),
            "-o",
            str(binary),
        ],
        text=True,
        capture_output=True,
    )
    if compile_result.returncode != 0:
        raise RuntimeError(
            f"COMPILE FAIL {name}\n{compile_result.stdout}{compile_result.stderr}"
        )
    run_result = subprocess.run([str(binary)], text=True, capture_output=True)
    if run_result.returncode != 0:
        raise RuntimeError(
            f"RUN FAIL {name} ({run_result.returncode})\n"
            f"{run_result.stdout}{run_result.stderr}"
        )
    output_text = run_result.stdout + run_result.stderr
    return name, output_text


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--jobs",
        type=int,
        default=int(os.environ.get("POKEPOD_TEST_JOBS", min(4, os.cpu_count() or 1))),
    )
    args = parser.parse_args()
    if args.jobs < 1:
        raise SystemExit("--jobs must be at least 1")
    compiler = shlex.split(os.environ.get("CXX", "clang++"))
    if not compiler or shutil.which(compiler[0]) is None:
        raise SystemExit(f"source-only gate requires a C++17 compiler: {compiler}")
    tests = sorted(TESTS.glob("test_*.cpp"))
    if not tests:
        raise SystemExit("no C++ host tests found")

    failures: list[str] = []
    results: dict[str, str] = {}
    with tempfile.TemporaryDirectory(prefix="pokepod-cpp-host-") as raw:
        output = Path(raw)
        with ThreadPoolExecutor(max_workers=args.jobs) as executor:
            futures = {
                executor.submit(execute, test, output, compiler): test.stem
                for test in tests
            }
            for future in as_completed(futures):
                name = futures[future]
                try:
                    resolved_name, text = future.result()
                    results[resolved_name] = text
                except Exception as exc:  # noqa: BLE001 - preserve all failures
                    failures.append(f"{name}: {exc}")
    for test in tests:
        name = test.stem
        if name in results:
            if results[name]:
                print(results[name], end="" if results[name].endswith("\n") else "\n")
            print(f"PASS {name}")
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    print(f"PASS cpp_host_gate ({len(tests)} tests, jobs={args.jobs})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
