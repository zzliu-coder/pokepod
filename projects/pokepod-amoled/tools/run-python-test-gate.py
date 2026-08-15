#!/usr/bin/env python3
"""Run one explicit Python host-test gate and reject unclassified tests."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import threading


TOOLS = Path(__file__).resolve().parent
MANIFEST = TOOLS / "test-gates.json"
VALID_GATES = ("source", "asset", "toolchain", "repository")
DEFAULT_TEST_TIMEOUT_SECONDS = 60.0


class TestFailure(RuntimeError):
    """One classified Python test failed or exceeded its explicit budget."""


_active_lock = threading.Lock()
_active_processes: set[subprocess.Popen[str]] = set()
_stop_requested = threading.Event()


def positive_timeout(raw: str) -> float:
    value = float(raw)
    if value <= 0:
        raise argparse.ArgumentTypeError("timeout must be greater than zero")
    return value


def terminate(process: subprocess.Popen[str]) -> None:
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


def cancel_active_processes() -> None:
    with _active_lock:
        processes = tuple(_active_processes)
    for process in processes:
        terminate(process)


def output_text(value: str | bytes | None) -> str:
    if value is None:
        return ""
    return value.decode(errors="replace") if isinstance(value, bytes) else value


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


def execute(name: str, timeout_seconds: float) -> tuple[str, str]:
    path = TOOLS / name
    command = [sys.executable, str(path)]
    if _stop_requested.is_set():
        raise TestFailure(f"CANCELLED test={name} stage=run after first failure")
    process = subprocess.Popen(
        command,
        cwd=TOOLS.parent,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=os.name == "posix",
    )
    with _active_lock:
        _active_processes.add(process)
    if _stop_requested.is_set():
        terminate(process)
        with _active_lock:
            _active_processes.discard(process)
        raise TestFailure(f"CANCELLED test={name} stage=run after first failure")
    try:
        try:
            stdout, stderr = process.communicate(timeout=timeout_seconds)
        except subprocess.TimeoutExpired as exc:
            terminate(process)
            stdout, stderr = process.communicate()
            captured = (
                output_text(exc.stdout)
                + output_text(exc.stderr)
                + stdout
                + stderr
            )
            raise TestFailure(
                f"TIMEOUT test={name} stage=run limit={timeout_seconds:g}s\n"
                f"COMMAND {shlex.join(command)}\n{captured}"
            ) from None
    finally:
        with _active_lock:
            _active_processes.discard(process)
    output = stdout + stderr
    if process.returncode != 0:
        raise TestFailure(
            f"FAIL test={name} stage=run exit={process.returncode}\n"
            f"COMMAND {shlex.join(command)}\n{output}"
        )
    return name, output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("gate", choices=VALID_GATES)
    parser.add_argument("--jobs", type=int)
    parser.add_argument(
        "--timeout-seconds",
        type=positive_timeout,
        default=positive_timeout(
            os.environ.get(
                "POKEPOD_PYTHON_TEST_TIMEOUT_SECONDS",
                str(DEFAULT_TEST_TIMEOUT_SECONDS),
            )
        ),
    )
    args = parser.parse_args()
    _stop_requested.clear()
    payload = load_manifest()
    validate(payload)
    names = payload[args.gate]
    assert isinstance(names, list)
    default_jobs = min(4, os.cpu_count() or 1) if args.gate == "source" else 1
    jobs = default_jobs if args.jobs is None else args.jobs
    if jobs < 1:
        raise SystemExit("--jobs must be at least 1")

    results: dict[str, str] = {}
    first_failure: str | None = None
    executor = ThreadPoolExecutor(max_workers=jobs)
    try:
        futures = {
            executor.submit(execute, str(name), args.timeout_seconds): str(name)
            for name in names
        }
        for future in as_completed(futures):
            try:
                name, output = future.result()
                results[name] = output
            except Exception as exc:  # noqa: BLE001 - first error is evidence
                first_failure = str(exc)
                _stop_requested.set()
                cancel_active_processes()
                for pending in futures:
                    pending.cancel()
                break
    finally:
        executor.shutdown(wait=True, cancel_futures=True)

    for name in names:
        output = results.get(str(name))
        if output is None:
            continue
        if output:
            print(output, end="" if output.endswith("\n") else "\n")
        print(f"PASS {name}")
    if first_failure is not None:
        print(first_failure, file=sys.stderr)
        return 1
    print(
        f"PASS python_test_gate_{args.gate} "
        f"({len(names)} tests, jobs={jobs}, timeout={args.timeout_seconds:g}s)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
