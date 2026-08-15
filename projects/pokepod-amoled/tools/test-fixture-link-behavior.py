#!/usr/bin/env python3
"""Behavior tests for Link identity and binary prepare handshakes."""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import pty
import select
import sys
import threading
import time


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "cdc-status.py"
spec = importlib.util.spec_from_file_location("pokepod_cdc_status", MODULE_PATH)
assert spec and spec.loader
cdc = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = cdc
spec.loader.exec_module(cdc)


def read_exact(fd: int, size: int, timeout: float = 2.0) -> bytes:
    deadline = time.monotonic() + timeout
    data = bytearray()
    while len(data) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("fake device read timeout")
        readable, _, _ = select.select([fd], [], [], min(remaining, 0.1))
        if not readable:
            continue
        chunk = os.read(fd, size - len(data))
        if not chunk:
            raise ConnectionError("fake device disconnected")
        data.extend(chunk)
    return bytes(data)


def read_frame(fd: int, timeout: float = 2.0):
    header = read_exact(fd, cdc.HEADER.size, timeout)
    magic, version, frame_type, flags, request_id, length, crc = cdc.HEADER.unpack(header)
    assert magic == cdc.MAGIC and version == cdc.VERSION
    payload = read_exact(fd, length, timeout)
    assert (cdc.binascii.crc32(payload) & 0xFFFFFFFF) == crc
    return frame_type, flags, request_id, payload


def send_frame(fd: int, frame_type: int, request_id: int, value: object) -> None:
    payload = value if isinstance(value, bytes) else json.dumps(
        value, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    encoded = cdc.encode_frame(frame_type, request_id, payload)
    offset = 0
    while offset < len(encoded):
        offset += os.write(fd, encoded[offset:])


def run_case(handler, client):
    master, slave = pty.openpty()
    port = os.ttyname(slave)
    errors: list[BaseException] = []

    def run_server() -> None:
        try:
            handler(master)
        except BaseException as error:
            errors.append(error)

    thread = threading.Thread(target=run_server, daemon=True)
    thread.start()
    try:
        result = client(port)
    finally:
        thread.join(timeout=4.0)
        os.close(master)
        os.close(slave)
    if thread.is_alive():
        raise TimeoutError("fake Link server did not finish")
    if errors:
        raise errors[0]
    return result


def identity_server(fd: int) -> None:
    frame_type, _, request_id, payload = read_frame(fd)
    assert frame_type == cdc.REQUEST_JSON
    assert json.loads(payload)["operation"] == "identity"
    send_frame(fd, cdc.RESPONSE_JSON, request_id, {
        "status": "ok", "version": 2,
        "deviceId": "pokepod-001122334455",
    })


def binary_server(fd: int, operation: str, expect_prepare: bool) -> None:
    frame_type, _, request_id, payload = read_frame(fd)
    assert frame_type == cdc.REQUEST_JSON
    request = json.loads(payload)
    assert request["operation"] == operation
    expected = int(request["binaryLength"])
    if expect_prepare:
        readable, _, _ = select.select([fd], [], [], 0.05)
        assert not readable, "firmware DATA arrived before prepare ACK"
        send_frame(fd, cdc.EVENT_JSON, request_id, {
            "event": "binary_ack", "received": 0,
        })
    else:
        # Font writes have no prepare ACK.  The host must send DATA promptly.
        readable, _, _ = select.select([fd], [], [], 0.5)
        assert readable, "font DATA did not arrive without a prepare ACK"
    received = 0
    while received < expected:
        frame_type, flags, incoming_id, chunk = read_frame(fd)
        assert frame_type == cdc.DATA and incoming_id == request_id
        received += len(chunk)
        assert bool(flags & 1) == (received == expected)
        send_frame(fd, cdc.EVENT_JSON, request_id, {
            "event": "binary_ack", "received": received,
        })
    send_frame(fd, cdc.RESPONSE_JSON, request_id, {
        "status": "ok", "version": 2,
    })


def main() -> int:
    identity = run_case(identity_server, lambda port: cdc.query(port, "identity", 2.0))
    assert identity["deviceId"] == "pokepod-001122334455"
    payload = bytes(range(200))
    font = run_case(
        lambda fd: binary_server(fd, "font-write", False),
        lambda port: cdc.query(port, "font-write", 2.0, payload),
    )
    assert font["status"] == "ok"
    firmware = run_case(
        lambda fd: binary_server(fd, "firmware-update", True),
        lambda port: cdc.query(
            port, "firmware-update", 2.0, payload,
            {"sha256": cdc.hashlib.sha256(payload).hexdigest()},
        ),
    )
    assert firmware["status"] == "ok"
    print("PASS fixture_link_identity_and_binary_handshakes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
