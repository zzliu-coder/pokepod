#!/usr/bin/env python3
"""Compile and execute PokePod's C++ host tests with bounded subprocesses."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading


PROJECT = Path(__file__).resolve().parents[1]
FIRMWARE = PROJECT / "firmware"
TESTS = FIRMWARE / "tests"
SOURCE = FIRMWARE / "PokePodAmoled"
SUPPORT = TESTS / "support"
DEFAULT_COMPILE_TIMEOUT_SECONDS = 60.0
DEFAULT_RUN_TIMEOUT_SECONDS = 60.0


class StageFailure(RuntimeError):
    """One named test failed in one explicit subprocess stage."""


_active_lock = threading.Lock()
_active_processes: set[subprocess.Popen[str]] = set()
_stop_requested = threading.Event()


def _positive_timeout(raw: str) -> float:
    value = float(raw)
    if value <= 0:
        raise argparse.ArgumentTypeError("timeout must be greater than zero")
    return value


def _terminate(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    try:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGTERM)
        else:
            process.terminate()
        process.wait(timeout=2)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            if os.name == "posix":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
        except ProcessLookupError:
            pass


def _cancel_active_processes() -> None:
    with _active_lock:
        processes = tuple(_active_processes)
    for process in processes:
        _terminate(process)


def _output_text(value: str | bytes | None) -> str:
    if value is None:
        return ""
    return value.decode(errors="replace") if isinstance(value, bytes) else value


def run_bounded(
    command: list[str], *, name: str, stage: str, timeout_seconds: float
) -> str:
    """Run one subprocess, reclaim its process group, and preserve diagnostics."""

    rendered = shlex.join(command)
    if _stop_requested.is_set():
        raise StageFailure(f"CANCELLED test={name} stage={stage} after first failure")
    process = subprocess.Popen(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=os.name == "posix",
    )
    with _active_lock:
        _active_processes.add(process)
    if _stop_requested.is_set():
        _terminate(process)
        with _active_lock:
            _active_processes.discard(process)
        raise StageFailure(f"CANCELLED test={name} stage={stage} after first failure")
    try:
        try:
            stdout, stderr = process.communicate(timeout=timeout_seconds)
        except subprocess.TimeoutExpired as exc:
            _terminate(process)
            stdout, stderr = process.communicate()
            captured = (
                _output_text(exc.stdout)
                + _output_text(exc.stderr)
                + stdout
                + stderr
            )
            raise StageFailure(
                f"TIMEOUT test={name} stage={stage} "
                f"limit={timeout_seconds:g}s\nCOMMAND {rendered}\n{captured}"
            ) from None
    finally:
        with _active_lock:
            _active_processes.discard(process)
    output = stdout + stderr
    if process.returncode != 0:
        raise StageFailure(
            f"FAIL test={name} stage={stage} exit={process.returncode}\n"
            f"COMMAND {rendered}\n{output}"
        )
    return output


def execute(
    test: Path,
    output: Path,
    compiler: list[str],
    compile_timeout_seconds: float,
    run_timeout_seconds: float,
) -> tuple[str, str]:
    name = test.stem
    binary = output / name
    compile_output = run_bounded(
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
        name=name,
        stage="compile",
        timeout_seconds=compile_timeout_seconds,
    )
    run_output = run_bounded(
        [str(binary)],
        name=name,
        stage="run",
        timeout_seconds=run_timeout_seconds,
    )
    return name, compile_output + run_output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--jobs",
        type=int,
        default=int(os.environ.get("POKEPOD_TEST_JOBS", min(4, os.cpu_count() or 1))),
    )
    parser.add_argument(
        "--compile-timeout-seconds",
        type=_positive_timeout,
        default=_positive_timeout(
            os.environ.get(
                "POKEPOD_CPP_COMPILE_TIMEOUT_SECONDS",
                str(DEFAULT_COMPILE_TIMEOUT_SECONDS),
            )
        ),
    )
    parser.add_argument(
        "--run-timeout-seconds",
        type=_positive_timeout,
        default=_positive_timeout(
            os.environ.get(
                "POKEPOD_CPP_RUN_TIMEOUT_SECONDS",
                str(DEFAULT_RUN_TIMEOUT_SECONDS),
            )
        ),
    )
    args = parser.parse_args()
    _stop_requested.clear()
    if args.jobs < 1:
        raise SystemExit("--jobs must be at least 1")
    compiler = shlex.split(os.environ.get("CXX", "clang++"))
    if not compiler or shutil.which(compiler[0]) is None:
        raise SystemExit(f"source-only gate requires a C++17 compiler: {compiler}")
    tests = sorted(path.resolve() for path in TESTS.glob("test_*.cpp"))
    if not tests:
        raise SystemExit("no C++ host tests found")

    results: dict[str, str] = {}
    first_failure: str | None = None
    with tempfile.TemporaryDirectory(prefix="pokepod-cpp-host-") as raw:
        output = Path(raw)
        executor = ThreadPoolExecutor(max_workers=args.jobs)
        try:
            futures = {
                executor.submit(
                    execute,
                    test,
                    output,
                    compiler,
                    args.compile_timeout_seconds,
                    args.run_timeout_seconds,
                ): test.stem
                for test in tests
            }
            for future in as_completed(futures):
                try:
                    resolved_name, text = future.result()
                    results[resolved_name] = text
                except Exception as exc:  # noqa: BLE001 - first error is evidence
                    first_failure = str(exc)
                    _stop_requested.set()
                    _cancel_active_processes()
                    for pending in futures:
                        pending.cancel()
                    break
        finally:
            executor.shutdown(wait=True, cancel_futures=True)
    for test in tests:
        name = test.stem
        if name in results:
            if results[name]:
                print(results[name], end="" if results[name].endswith("\n") else "\n")
            print(f"PASS {name}")
    if first_failure is not None:
        print(first_failure, file=sys.stderr)
        return 1
    print(
        f"PASS cpp_host_gate ({len(tests)} tests, jobs={args.jobs}, "
        f"compile_timeout={args.compile_timeout_seconds:g}s, "
        f"run_timeout={args.run_timeout_seconds:g}s)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
