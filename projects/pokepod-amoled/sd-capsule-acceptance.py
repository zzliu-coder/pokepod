#!/usr/bin/python3
"""Record, verify, and remove one PokePod SD capsule over Link v2."""

from __future__ import annotations

import argparse
import array
import glob
import importlib.util
import io
import json
import pathlib
import sys
import time
import uuid
import wave


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent


def load_link_module():
    path = SCRIPT_DIR / "cdc-status.py"
    spec = importlib.util.spec_from_file_location("pokepod_link_probe", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load cdc-status.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


LINK = load_link_module()


class Device:
    def __init__(self, ports: list[str], timeout: float):
        self.timeout = timeout
        self.port = self._find_port(ports)

    def _find_port(self, ports: list[str]) -> str:
        errors = []
        for port in ports:
            try:
                response = LINK.query(port, "status", self.timeout)
                if response.get("status") == "ok" and response.get("sdReady"):
                    return port
                errors.append(f"{port}: {response}")
            except Exception as error:  # Hardware discovery must try every port.
                errors.append(f"{port}: {error}")
        raise RuntimeError("no SD-ready PokePod CDC port: " + "; ".join(errors))

    def call(self, operation: str, *, binary: bytes | None = None,
             fields: dict[str, object] | None = None):
        deadline = time.monotonic() + self.timeout
        while True:
            response = LINK.query(
                self.port, operation, self.timeout,
                outgoing_binary=binary, fields=fields,
            )
            if response.get("status") == "ok":
                return response
            if response.get("status") != "busy" or time.monotonic() >= deadline:
                raise RuntimeError(
                    f"{operation} failed: {response.get('message', response)}"
                )
            retry_ms = max(20, min(1000, int(response.get("retryAfterMs", 150))))
            time.sleep(retry_ms / 1000)

    def read(self, path: str) -> bytes:
        response = self.call("read", fields={"path": path})
        payload = response.get("_binary_payload")
        if not isinstance(payload, bytes):
            raise RuntimeError(f"read returned no binary payload for {path}")
        return payload

    def list_files(self) -> list[str]:
        files = []
        cursor = None
        while True:
            fields: dict[str, object] = {"path": ".", "recursive": True}
            if cursor is not None:
                fields["cursor"] = cursor
            response = self.call("read", fields=fields)
            files.extend(item["path"] for item in response.get("files", []))
            cursor = response.get("nextCursor")
            if cursor is None:
                return files

    def command(self, operation: str, maintenance_id: str, **fields):
        transaction_id = str(uuid.uuid4())
        command = {
            "schemaVersion": 2,
            "transactionId": transaction_id,
            "operation": operation,
            "maintenanceId": maintenance_id,
            **fields,
        }
        self.call(
            "command",
            binary=json.dumps(
                command, separators=(",", ":"), sort_keys=True
            ).encode("utf-8"),
            fields={"transactionId": transaction_id},
        )
        result = self.call("result", fields={"transactionId": transaction_id})
        payload = result.get("_binary_payload")
        if not isinstance(payload, bytes):
            raise RuntimeError(f"{operation} returned no command result")
        decoded = json.loads(payload.decode("utf-8"))
        if not decoded.get("success"):
            raise RuntimeError(f"{operation} rejected: {decoded.get('message')}")
        return decoded


def decode_json(payload: bytes, label: str):
    try:
        decoded = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RuntimeError(f"invalid {label}: {error}") from error
    if not isinstance(decoded, dict):
        raise RuntimeError(f"invalid {label}: root must be an object")
    return decoded


def verify_wav(payload: bytes, minimum_seconds: float):
    try:
        with wave.open(io.BytesIO(payload), "rb") as recording:
            channels = recording.getnchannels()
            sample_width = recording.getsampwidth()
            sample_rate = recording.getframerate()
            frames = recording.getnframes()
            pcm = recording.readframes(frames)
    except (wave.Error, EOFError) as error:
        raise RuntimeError(f"invalid audio.wav: {error}") from error
    duration = frames / sample_rate if sample_rate else 0
    if (channels, sample_width, sample_rate) != (1, 2, 16_000):
        raise RuntimeError(
            "unexpected WAV format "
            f"channels={channels} width={sample_width} rate={sample_rate}"
        )
    if duration < minimum_seconds:
        raise RuntimeError(f"WAV is too short: {duration:.3f}s")
    samples = array.array("h")
    samples.frombytes(pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    peak = max((abs(value) for value in samples), default=0)
    if peak == 0:
        raise RuntimeError("WAV contains only digital silence")
    return {
        "sampleRateHz": sample_rate,
        "channels": channels,
        "bitsPerSample": sample_width * 8,
        "durationSeconds": round(duration, 3),
        "peak": peak,
        "bytes": len(payload),
    }


def remove_capsule(device: Device, capsule_id: str, revision: int):
    maintenance_id = str(uuid.uuid4())
    maintenance_open = False
    try:
        device.command("beginMaintenance", maintenance_id)
        maintenance_open = True
        device.command(
            "deleteCapsules", maintenance_id,
            capsuleIds=[capsule_id],
            expectedRevisions={capsule_id: revision},
        )
        trash = decode_json(
            device.read(f".trash/{capsule_id}/trash.json"), "trash.json"
        )
        device.command(
            "purgeCapsules", maintenance_id,
            capsuleIds=[capsule_id],
            expectedRevisions={capsule_id: int(trash["revision"])},
        )
    finally:
        if maintenance_open:
            device.command("endMaintenance", maintenance_id)


def cleanup_capsule(device: Device, capsule_id: str):
    files = set(device.list_files())
    active_metadata = f"Inbox/{capsule_id}/capsule.json"
    trash_metadata = f".trash/{capsule_id}/trash.json"
    if active_metadata in files:
        capsule = decode_json(device.read(active_metadata), "capsule.json")
        remove_capsule(device, capsule_id, int(capsule["revision"]))
        return
    if trash_metadata in files:
        trash = decode_json(device.read(trash_metadata), "trash.json")
        maintenance_id = str(uuid.uuid4())
        maintenance_open = False
        try:
            device.command("beginMaintenance", maintenance_id)
            maintenance_open = True
            device.command(
                "purgeCapsules", maintenance_id,
                capsuleIds=[capsule_id],
                expectedRevisions={capsule_id: int(trash["revision"])},
            )
        finally:
            if maintenance_open:
                device.command("endMaintenance", maintenance_id)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("ports", nargs="*")
    parser.add_argument("--record-seconds", type=float, default=3.0)
    parser.add_argument("--timeout", type=float, default=8.0)
    arguments = parser.parse_args()
    if not 1.0 <= arguments.record_seconds <= 10.0:
        parser.error("--record-seconds must be between 1 and 10")

    ports = arguments.ports or sorted(glob.glob("/dev/cu.usbmodem*"))
    device = Device(ports, arguments.timeout)
    baseline = set(device.list_files())
    started = device.call("record")
    capsule_id = str(started.get("capsuleId", "")).lower()
    if not capsule_id:
        raise RuntimeError("record returned no capsuleId")
    recording = True
    cleaned = False
    try:
        time.sleep(arguments.record_seconds)
        device.call("stop")
        recording = False

        prefix = f"Inbox/{capsule_id}/"
        files = set(device.list_files())
        expected = {
            prefix + "audio.wav",
            prefix + "capsule.json",
            prefix + "processing.json",
        }
        if not expected.issubset(files):
            raise RuntimeError(
                f"committed capsule is incomplete: {sorted(files - baseline)}"
            )
        capsule = decode_json(device.read(prefix + "capsule.json"), "capsule.json")
        processing = decode_json(
            device.read(prefix + "processing.json"), "processing.json"
        )
        if str(capsule.get("id", "")).lower() != capsule_id:
            raise RuntimeError("capsule.json id does not match directory")
        expected_processing = {
            "schemaVersion": 2,
            "capsuleId": capsule_id,
            "audioFile": "audio.wav",
            "audioFormat": "wav-pcm-s16le",
            "sampleRateHz": 16_000,
            "channels": 1,
            "bitsPerSample": 16,
        }
        normalized = dict(processing)
        normalized["capsuleId"] = str(
            normalized.get("capsuleId", "")
        ).lower()
        for key, value in expected_processing.items():
            if normalized.get(key) != value:
                raise RuntimeError(
                    f"processing.json {key}={normalized.get(key)!r}; "
                    f"expected {value!r}"
                )
        audio = verify_wav(
            device.read(prefix + "audio.wav"),
            max(0.8, arguments.record_seconds - 1.0),
        )
        remove_capsule(device, capsule_id, int(capsule["revision"]))
        cleaned = True
        remaining = set(device.list_files())
        if remaining != baseline:
            raise RuntimeError(
                "SD visible contents were not restored after test: "
                f"{sorted(remaining.symmetric_difference(baseline))}"
            )
        status = device.call("status")
        if int(status.get("pendingCapsules", -1)) != 0:
            raise RuntimeError("pending capsule count did not return to zero")
        print(json.dumps({
            "result": "PASS",
            "test": "sd_capsule_acceptance",
            "port": device.port,
            "capsuleId": capsule_id,
            "processingSchema": processing["schemaVersion"],
            "audio": audio,
            "visibleFilesAfterCleanup": sorted(remaining),
        }, separators=(",", ":"), sort_keys=True))
        return 0
    finally:
        if recording:
            try:
                device.call("stop")
            except Exception:
                pass
        if not cleaned and capsule_id:
            try:
                cleanup_capsule(device, capsule_id)
            except Exception as error:
                print(f"automatic capsule cleanup failed: {error}", file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
