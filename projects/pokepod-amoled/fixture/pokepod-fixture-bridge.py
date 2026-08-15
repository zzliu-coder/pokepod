#!/usr/bin/env python3
"""Drive the reference USB BOOT/RESET/power bridge with explicit commands."""

from __future__ import annotations

import argparse
import json
import sys
import time

import serial


COMMANDS = {
    "assert-boot": "BOOT ASSERT",
    "release-boot": "BOOT RELEASE",
    "pulse-reset": "RESET PULSE",
    "power-off": "POWER OFF",
    "power-on": "POWER ON",
    "ping": "PING",
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--action", choices=tuple(COMMANDS), required=True)
    parser.add_argument("--timeout", type=float, default=2.0)
    args = parser.parse_args()
    if not 0.1 <= args.timeout <= 10.0:
        parser.error("--timeout must be between 0.1 and 10 seconds")
    try:
        with serial.Serial(args.port, 115200, timeout=0.1,
                           write_timeout=args.timeout) as connection:
            connection.reset_input_buffer()
            connection.write((COMMANDS[args.action] + "\n").encode("ascii"))
            connection.flush()
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                line = connection.readline().decode("utf-8", "replace").strip()
                if not line:
                    continue
                if line == "OK":
                    print(json.dumps({"ok": True, "action": args.action}))
                    return 0
                if line.startswith("ERROR"):
                    print(line, file=sys.stderr)
                    return 2
        print("fixture bridge response timeout", file=sys.stderr)
        return 3
    except (OSError, serial.SerialException) as error:
        print(f"fixture bridge unavailable: {error}", file=sys.stderr)
        return 4


if __name__ == "__main__":
    raise SystemExit(main())

