#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import time


TOOLS = Path(__file__).resolve().parent


def load(name: str, path: Path):
    specification = importlib.util.spec_from_file_location(name, path)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


cpp = load("pokepod_cpp_runner", TOOLS / "run-cpp-host-tests.py")
python_gate = load("pokepod_python_gate", TOOLS / "run-python-test-gate.py")
cpp._stop_requested.clear()
python_gate._stop_requested.clear()

with tempfile.TemporaryDirectory(prefix="pokepod-runner-timeout-") as raw:
    root = Path(raw)
    ok = root / "ok.py"
    ok.write_text("print('ok')\n", encoding="utf-8")
    output = cpp.run_bounded(
        [sys.executable, str(ok)], name="normal", stage="run", timeout_seconds=2
    )
    assert output == "ok\n"

    failed = root / "failed.py"
    failed.write_text("raise SystemExit(7)\n", encoding="utf-8")
    try:
        cpp.run_bounded(
            [sys.executable, str(failed)],
            name="ordinary_failure",
            stage="compile",
            timeout_seconds=2,
        )
    except cpp.StageFailure as error:
        message = str(error)
        assert "test=ordinary_failure" in message
        assert "stage=compile" in message
        assert "exit=7" in message
        assert "COMMAND" in message
    else:
        raise AssertionError("ordinary failure was accepted")

    hanging = root / "hanging.py"
    hanging.write_text(
        "import subprocess, sys, time\n"
        "subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])\n"
        "time.sleep(30)\n",
        encoding="utf-8",
    )
    started = time.monotonic()
    try:
        cpp.run_bounded(
            [sys.executable, str(hanging)],
            name="run_timeout",
            stage="run",
            timeout_seconds=0.2,
        )
    except cpp.StageFailure as error:
        message = str(error)
        assert "TIMEOUT test=run_timeout stage=run limit=0.2s" in message
    else:
        raise AssertionError("C++ runner timeout was accepted")
    assert time.monotonic() - started < 5

    original_tools = python_gate.TOOLS
    try:
        python_gate.TOOLS = root
        python_hanging = root / "test-python-timeout.py"
        python_hanging.write_text("import time; time.sleep(30)\n", encoding="utf-8")
        started = time.monotonic()
        try:
            python_gate.execute(python_hanging.name, 0.2)
        except python_gate.TestFailure as error:
            message = str(error)
            assert "TIMEOUT test=test-python-timeout.py stage=run limit=0.2s" in message
            assert "COMMAND" in message
        else:
            raise AssertionError("Python runner timeout was accepted")
        assert time.monotonic() - started < 5
    finally:
        python_gate.TOOLS = original_tools

for runner in ("run-cpp-host-tests.py", "run-python-test-gate.py"):
    source = (TOOLS / runner).read_text(encoding="utf-8")
    assert "first_failure" in source
    assert "cancel_active_processes" in source or "_cancel_active_processes" in source
print("PASS test-test-runner-timeouts")
