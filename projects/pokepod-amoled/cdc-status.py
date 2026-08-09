#!/usr/bin/python3
"""Small PokePod Link v2 probe used by the hardware acceptance scripts."""

from __future__ import annotations

import argparse
import binascii
import glob
import json
import os
import select
import struct
import sys
import termios
import time


MAGIC = b"PPV2"
VERSION = 2
REQUEST_JSON = 1
RESPONSE_JSON = 2
DATA = 3
EVENT_JSON = 4
HEADER = struct.Struct("<4sBBHIII")
MAX_CONTROL = 4096
MAX_DATA = 16384
OUTGOING_CHUNK = 128
OUTGOING_PACE_SECONDS = 0.001


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


def write_all(fd: int, value: bytes, timeout: float = 30.0) -> None:
    offset = 0
    deadline = time.monotonic() + timeout
    while offset < len(value):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("PokePod CDC write timed out")
        _, writable, _ = select.select([], [fd], [], min(remaining, 0.25))
        if not writable:
            continue
        try:
            written = os.write(fd, value[offset:])
        except BlockingIOError:
            continue
        if written <= 0:
            raise ConnectionError("PokePod CDC disconnected while writing")
        offset += written


def read_exact(fd: int, length: int, deadline: float) -> bytes:
    result = bytearray()
    while len(result) < length:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("PokePod Link v2 timed out")
        readable, _, _ = select.select([fd], [], [], min(remaining, 0.25))
        if not readable:
            continue
        chunk = os.read(fd, length - len(result))
        if not chunk:
            raise ConnectionError("PokePod CDC disconnected")
        result.extend(chunk)
    return bytes(result)


def encode_frame(frame_type: int, request_id: int, payload: bytes,
                 flags: int = 0) -> bytes:
    limit = MAX_DATA if frame_type == DATA else MAX_CONTROL
    if len(payload) > limit:
        raise ValueError("Link v2 payload is too large")
    crc = binascii.crc32(payload) & 0xFFFFFFFF
    return HEADER.pack(MAGIC, VERSION, frame_type, flags, request_id,
                       len(payload), crc) + payload


def read_frame(fd: int, deadline: float):
    raw = read_exact(fd, HEADER.size, deadline)
    magic, version, frame_type, flags, request_id, length, expected_crc = (
        HEADER.unpack(raw)
    )
    limit = MAX_DATA if frame_type == DATA else MAX_CONTROL
    if magic != MAGIC or version != VERSION or frame_type not in (1, 2, 3, 4):
        raise ValueError("invalid PokePod Link v2 header")
    if length > limit:
        raise ValueError("PokePod Link v2 payload is too large")
    payload = read_exact(fd, length, deadline)
    if (binascii.crc32(payload) & 0xFFFFFFFF) != expected_crc:
        raise ValueError("PokePod Link v2 CRC mismatch")
    return frame_type, flags, request_id, payload


def query(port: str, operation: str, timeout: float,
          outgoing_binary: bytes | None = None,
          fields: dict[str, object] | None = None):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure(fd)
        request_id = (time.monotonic_ns() & 0xFFFFFFFF) or 1
        request = dict(fields or {})
        request["operation"] = operation
        request["version"] = VERSION
        if outgoing_binary is not None:
            request["binaryLength"] = len(outgoing_binary)
            request["chunkAcks"] = True
        control = json.dumps(
            request,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
        write_all(fd, encode_frame(REQUEST_JSON, request_id, control))
        if outgoing_binary is not None:
            # Give the 256-byte TinyUSB CDC RX window one poll cycle before
            # the first data frame. Each framed chunk then remains below that
            # window as well.
            time.sleep(0.010)
            offset = 0
            while offset < len(outgoing_binary):
                # macOS can buffer CDC writes much faster than the ESP32 can
                # persist them to SD.  Keep Link v2 frames comfortably below
                # the TinyUSB RX window and pace them so the device never has
                # to recover from a silently dropped frame.
                chunk = outgoing_binary[offset:offset + OUTGOING_CHUNK]
                offset += len(chunk)
                write_all(fd, encode_frame(
                    DATA, request_id, chunk,
                    flags=1 if offset == len(outgoing_binary) else 0,
                ))
                termios.tcdrain(fd)
                time.sleep(OUTGOING_PACE_SECONDS)
                ack_deadline = time.monotonic() + max(5.0, timeout)
                while True:
                    frame_type, _, incoming_id, ack_payload = read_frame(
                        fd, ack_deadline
                    )
                    if incoming_id != request_id:
                        continue
                    if frame_type == RESPONSE_JSON:
                        rejected = json.loads(ack_payload.decode("utf-8"))
                        raise ValueError(rejected.get(
                            "message", "binary transfer rejected"
                        ))
                    if frame_type != EVENT_JSON:
                        continue
                    ack = json.loads(ack_payload.decode("utf-8"))
                    if (ack.get("event") != "binary_ack" or
                            int(ack.get("received", -1)) != offset):
                        raise ValueError(
                            "invalid Link v2 binary acknowledgement"
                        )
                    break
            if not outgoing_binary:
                write_all(fd, encode_frame(DATA, request_id, b"", flags=1))
        deadline = time.monotonic() + timeout
        response = None
        binary = bytearray()
        expected_binary = 0
        while time.monotonic() < deadline:
            frame_type, _, incoming_id, payload = read_frame(fd, deadline)
            if incoming_id != request_id:
                continue
            if frame_type == RESPONSE_JSON:
                if response is not None:
                    raise ValueError("duplicate Link v2 response")
                response = json.loads(payload.decode("utf-8"))
                expected_binary = int(response.get("binaryLength", 0))
            elif frame_type == DATA:
                binary.extend(payload)
            if response is not None and len(binary) >= expected_binary:
                if len(binary) != expected_binary:
                    raise ValueError("Link v2 binary length mismatch")
                if expected_binary:
                    response["_binary_payload"] = bytes(binary)
                response["host_port"] = port
                return response
        raise TimeoutError("PokePod Link v2 response timed out")
    finally:
        os.close(fd)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("ports", nargs="*")
    parser.add_argument("--command", default="status",
                        choices=("hello", "status", "record", "stop",
                                 "reboot"))
    parser.add_argument(
        "--install-font", metavar="PATH",
        help="install a PKF2 20px A4 font over PokePod Link v2",
    )
    parser.add_argument("--event", default="")  # legacy script compatibility
    parser.add_argument("--timeout", type=float, default=3.0)
    arguments = parser.parse_args()
    outgoing_binary = None
    operation = arguments.command
    if arguments.install_font:
        operation = "font-write"
        try:
            with open(arguments.install_font, "rb") as font_file:
                outgoing_binary = font_file.read()
        except OSError as error:
            parser.error(str(error))
        if not (20 <= len(outgoing_binary) <= 5 * 1024 * 1024):
            parser.error("font file must be between 20 bytes and 5 MiB")
    ports = arguments.ports or sorted(glob.glob("/dev/cu.usbmodem*"))
    last_error = None
    for port in ports:
        try:
            result = query(port, operation, arguments.timeout, outgoing_binary)
            if result.get("status") != "ok":
                last_error = result.get("message", result.get("status", "error"))
                continue
            if arguments.event and result.get("event") != arguments.event:
                last_error = f"unexpected event: {result.get('event')!r}"
                continue
        except (OSError, ValueError, TimeoutError, ConnectionError,
                UnicodeDecodeError, json.JSONDecodeError) as error:
            last_error = str(error)
            continue
        print(json.dumps(result, separators=(",", ":"), sort_keys=True))
        return 0
    print(f"PokePod Link v2 did not answer: {last_error or 'no CDC port'}",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
