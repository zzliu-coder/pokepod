#!/usr/bin/env python3
"""Import a fixed Chinese WAV, verify device-side Tencent ASR, then clean up."""

from __future__ import annotations

import argparse
import datetime
import glob
import importlib.util
import json
import pathlib
import subprocess
import tempfile
import time
import uuid


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
CAPSULE_SCRIPT = SCRIPT_DIR / "sd-capsule-acceptance.py"


def load_capsule_module():
    spec = importlib.util.spec_from_file_location("pokepod_capsule", CAPSULE_SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load sd-capsule-acceptance.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CAPSULE = load_capsule_module()


def encode(value: dict[str, object]) -> bytes:
    return (json.dumps(value, ensure_ascii=False, separators=(",", ":"),
                       sort_keys=True) + "\n").encode("utf-8")


def submit_command(device, command: dict[str, object]):
    transaction_id = str(command["transactionId"])
    device.call("command", binary=encode(command),
                fields={"transactionId": transaction_id})
    result = device.call("result", fields={"transactionId": transaction_id})
    payload = result.get("_binary_payload")
    if not isinstance(payload, bytes):
        raise RuntimeError("commitImport returned no command result")
    decoded = CAPSULE.decode_json(payload, "command result")
    if not decoded.get("success"):
        raise RuntimeError(f"commitImport rejected: {decoded.get('message')}")


def make_fixture(directory: pathlib.Path, phrase: str) -> bytes:
    source = directory / "fixture.aiff"
    wav = directory / "audio.wav"
    subprocess.run(["say", "-v", "Tingting", "-r", "135", "-o", str(source),
                    phrase], check=True)
    subprocess.run([
        "ffmpeg", "-y", "-nostdin", "-hide_banner", "-loglevel", "error",
        "-i", str(source), "-af", "loudnorm=I=-18:TP=-2:LRA=7",
        "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le", str(wav),
    ], check=True)
    payload = wav.read_bytes()
    CAPSULE.verify_wav(payload, 2.0)
    return payload


def import_capsule(device, capsule_id: str, audio: bytes, duration_ms: int):
    transaction_id = str(uuid.uuid4())
    maintenance_id = str(uuid.uuid4())
    now = datetime.datetime.now(datetime.timezone.utc).replace(
        microsecond=0).isoformat().replace("+00:00", "Z")
    capsule = {
        "schemaVersion": 1, "id": capsule_id, "title": "腾讯云自动验收",
        "createdAt": now, "updatedAt": now, "revision": 1,
        "favorite": False, "tags": [], "language": "zh", "contentHash": None,
    }
    processing = {
        "schemaVersion": 2, "capsuleId": capsule_id, "revision": 1,
        "durationMs": duration_ms, "status": "queued", "audioFile": "audio.wav",
        "audioFormat": "wav-pcm-s16le", "sampleRateHz": 16000, "channels": 1,
        "bitsPerSample": 16, "rawTextFile": None, "polishedTextFile": None,
        "errorStage": None, "error": None, "attempts": 0,
        "engine": "tencent-asr", "model": "16k_zh",
    }
    device.command("beginMaintenance", maintenance_id)
    maintenance_open = True
    released_at = 0.0
    try:
        for path, payload in (("capsule.json", encode(capsule)),
                              ("processing.json", encode(processing)),
                              ("audio.wav", audio)):
            device.call("stage-write", binary=payload, fields={
                "transactionId": transaction_id, "capsuleId": capsule_id,
                "path": path,
            })
        device.call("commit", fields={
            "transactionId": transaction_id, "capsuleId": capsule_id,
            "stagingOnly": True,
        })
        submit_command(device, {
            "schemaVersion": 2, "transactionId": transaction_id,
            "operation": "commitImport", "maintenanceId": maintenance_id,
            "capsuleIds": [capsule_id], "destination": "Inbox",
            "stagedPath": f".staging/{transaction_id}/{capsule_id}",
        })
    finally:
        if maintenance_open:
            released_at = time.monotonic()
            device.command("endMaintenance", maintenance_id)
    return released_at


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("ports", nargs="*")
    parser.add_argument("--timeout", type=float, default=30.0)
    arguments = parser.parse_args()
    ports = arguments.ports or sorted(glob.glob("/dev/cu.usbmodem*"))
    device = CAPSULE.Device(ports, 8)
    baseline = set(device.list_files())
    capsule_id = str(uuid.uuid4())
    phrase = "这是腾讯云自动转写测试，今天网络连接正常。"
    cleaned = False
    try:
        with tempfile.TemporaryDirectory(prefix="pokepod-tencent-") as temporary:
            audio = make_fixture(pathlib.Path(temporary), phrase)
        duration_ms = int((len(audio) - 44) * 1000 / 32000)
        import_started = time.monotonic()
        started = import_capsule(device, capsule_id, audio, duration_ms)
        import_seconds = started - import_started
        prefix = f"Inbox/{capsule_id}/"
        deadline = time.monotonic() + arguments.timeout
        last_status = None
        while time.monotonic() < deadline:
            status = device.call("status")
            last_status = status
            if int(status.get("pendingCapsules", -1)) == 0 and not status.get(
                    "transcribing", False):
                break
            time.sleep(0.1)
        else:
            raise RuntimeError(f"Tencent ASR timed out: {last_status}")
        elapsed = time.monotonic() - started
        processing = CAPSULE.decode_json(
            device.read(prefix + "processing.json"), "processing.json")
        if processing.get("status") not in ("raw_ready", "ready"):
            raise RuntimeError(
                f"Tencent ASR failed: {processing.get('error', processing)}")
        raw = device.read(prefix + "raw.txt").decode("utf-8").strip()
        for expected in ("腾讯云", "网络连接"):
            if expected not in raw:
                raise RuntimeError(f"ASR text lacks {expected!r}: {raw!r}")
        diagnostics = device.call("status")
        CAPSULE.cleanup_capsule(device, capsule_id)
        cleaned = True
        remaining = set(device.list_files())
        if remaining != baseline:
            raise RuntimeError("visible SD contents changed after fixture cleanup")
        print(json.dumps({
            "result": "PASS", "test": "tencent_e2e_acceptance",
            "port": device.port, "audioDurationMs": duration_ms,
            "importSeconds": round(import_seconds, 3),
            "elapsedSeconds": round(elapsed, 3), "rawText": raw,
            "stagesMs": {
                "hash": diagnostics.get("asr_hash_ms"),
                "connect": diagnostics.get("asr_connect_ms"),
                "upload": diagnostics.get("asr_upload_ms"),
                "total": diagnostics.get("asr_total_ms"),
            },
        }, ensure_ascii=False, separators=(",", ":"), sort_keys=True))
        return 0
    finally:
        if not cleaned:
            try:
                CAPSULE.cleanup_capsule(device, capsule_id)
            except Exception as error:
                print(f"automatic fixture cleanup failed: {error}")


if __name__ == "__main__":
    raise SystemExit(main())
