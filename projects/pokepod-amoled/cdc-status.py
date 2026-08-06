#!/usr/bin/python3

import argparse
import glob
import json
import os
import select
import sys
import termios
import time


def configure(fd: int) -> None:
    attributes = termios.tcgetattr(fd)
    attributes[0] = 0
    attributes[1] = 0
    attributes[2] = termios.CLOCAL | termios.CREAD | termios.CS8
    attributes[3] = 0
    attributes[4] = termios.B115200
    attributes[5] = termios.B115200
    attributes[6][termios.VMIN] = 0
    attributes[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attributes)
    termios.tcflush(fd, termios.TCIOFLUSH)


def query(port: str, command: str, expected_event: str, timeout: float):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure(fd)
        time.sleep(0.15)
        os.write(fd, command.encode("ascii") + b"\n")
        deadline = time.monotonic() + timeout
        buffered = b""
        while time.monotonic() < deadline:
            readable, _, _ = select.select([fd], [], [], 0.2)
            if not readable:
                continue
            chunk = os.read(fd, 4096)
            if not chunk:
                continue
            buffered += chunk
            while b"\n" in buffered:
                raw, buffered = buffered.split(b"\n", 1)
                try:
                    value = json.loads(raw.decode("utf-8", errors="strict"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                if value.get("event") == expected_event:
                    value["host_port"] = port
                    return value
        return None
    finally:
        os.close(fd)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("ports", nargs="*")
    parser.add_argument("--command", default="status")
    parser.add_argument("--event", default="status")
    parser.add_argument("--timeout", type=float, default=3.0)
    arguments = parser.parse_args()
    ports = arguments.ports or sorted(glob.glob("/dev/cu.usbmodem*"))
    for port in ports:
        try:
            result = query(port, arguments.command, arguments.event,
                           arguments.timeout)
        except OSError:
            continue
        if result is not None:
            print(json.dumps(result, separators=(",", ":"), sort_keys=True))
            return 0
    print("PokePod diagnostics port did not answer", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
