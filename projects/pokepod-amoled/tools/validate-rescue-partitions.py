#!/usr/bin/env python3
"""Validate and model the PokePod ESP32-S3 ROM rescue layout.

The ROM rescue path is deliberately kept independent from the firmware
implementation.  It consumes the partition table read from the device, and
refuses to produce a write plan until the table, slot policy and OTA selector
are internally consistent.  This gives the shell script a small, testable
authority for the dangerous part of a rescue operation.
"""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
from pathlib import Path
import re
import struct
from typing import Iterable


PARTITION_TABLE_BYTES = 0x1000
PARTITION_TABLE_ENTRY_BYTES = 32
PARTITION_TABLE_ENTRIES_BYTES = 0xC00
PARTITION_MAGIC = 0x50AA
PARTITION_MD5_MAGIC = 0xEBEB
PARTITION_TERMINATORS = {0x0000, 0xFFFF}
FLASH_BYTES = 16 * 1024 * 1024
OTADATA_BYTES = 0x2000
APP_SLOT_BYTES = 0x300000
OTADATA_ENTRY_BYTES = 32
OTADATA_COPIES = 2
OTADATA_INVALID_SEQUENCE = 0xFFFFFFFF

ENTRY = struct.Struct("<HBBII16sI")

EXPECTED = {
    "otadata": {"type": 0x01, "subtype": 0x00, "offset": 0xE000, "size": 0x2000},
    "app0": {"type": 0x00, "subtype": 0x10, "offset": 0x10000, "size": 0x300000},
    "app1": {"type": 0x00, "subtype": 0x11, "offset": 0x310000, "size": 0x300000},
}


def fail(message: str) -> None:
    raise SystemExit(f"FAIL rescue_partition_layout {message}")


def _decode_label(raw: bytes, cursor: int) -> str:
    if b"\0" in raw:
        raw = raw.split(b"\0", 1)[0]
    try:
        label = raw.decode("ascii")
    except UnicodeDecodeError:
        fail(f"non_ascii_label offset=0x{cursor:x}")
    if not label or len(label) > 15 or any(char.isspace() for char in label):
        fail(f"invalid_label offset=0x{cursor:x}")
    return label


def parse(payload: bytes) -> dict[str, dict[str, int]]:
    """Parse one exact 0x1000-byte ESP-IDF partition-table image."""

    if len(payload) != PARTITION_TABLE_BYTES:
        fail(
            f"partition_table_size expected=0x{PARTITION_TABLE_BYTES:x} "
            f"actual=0x{len(payload):x}"
        )

    observed: dict[str, dict[str, int]] = {}
    saw_end = False
    saw_md5 = False
    for cursor in range(0, PARTITION_TABLE_ENTRIES_BYTES, PARTITION_TABLE_ENTRY_BYTES):
        block = payload[cursor : cursor + ENTRY.size]
        if len(block) != ENTRY.size:
            fail(f"partition_table_truncated offset=0x{cursor:x}")
        magic, part_type, subtype, offset, size, raw_label, flags = ENTRY.unpack(block)
        if magic in PARTITION_TERMINATORS:
            saw_end = True
            break
        if magic == PARTITION_MD5_MAGIC:
            # An ESP-IDF MD5 record occupies a normal entry: bytes 16..31 are
            # the digest of every partition entry before this record.
            if payload[cursor + 16 : cursor + 32] != hashlib.md5(payload[:cursor]).digest():
                fail(f"partition_table_md5_mismatch offset=0x{cursor:x}")
            saw_md5 = True
            break
        if magic != PARTITION_MAGIC:
            fail(f"invalid_magic value=0x{magic:04x} offset=0x{cursor:x}")
        if size <= 0:
            fail(f"zero_size offset=0x{cursor:x}")
        label = _decode_label(raw_label, cursor)
        if label in observed:
            fail(f"duplicate_label label={label}")
        if offset % 0x1000 != 0 or size % 0x1000 != 0:
            fail(f"unaligned_partition label={label}")
        if offset < 0 or offset + size > FLASH_BYTES:
            fail(f"partition_out_of_flash label={label}")
        observed[label] = {
            "type": part_type,
            "subtype": subtype,
            "offset": offset,
            "size": size,
            "flags": flags,
        }

    if not observed:
        fail("partition_table_empty")
    if not saw_end and not saw_md5:
        fail("partition_table_truncated_or_unterminated")

    intervals: list[tuple[int, int, str]] = []
    for label, part in observed.items():
        intervals.append((part["offset"], part["offset"] + part["size"], label))
    intervals.sort()
    for previous, current in zip(intervals, intervals[1:]):
        if current[0] < previous[1]:
            fail(
                f"overlap first={previous[2]} second={current[2]} "
                f"at=0x{current[0]:x}"
            )
    return observed


def validate_layout(observed: dict[str, dict[str, int]]) -> dict[str, dict[str, int]]:
    for label, expected in EXPECTED.items():
        actual = observed.get(label)
        if actual is None:
            fail(f"missing label={label}")
        for field, value in expected.items():
            if actual.get(field) != value:
                fail(
                    f"{label}.{field} expected=0x{value:x} "
                    f"actual=0x{actual.get(field, -1):x}"
                )
    return observed


def load_and_validate(path: Path) -> dict[str, dict[str, int]]:
    try:
        payload = path.read_bytes()
    except OSError as error:
        fail(f"partition_table_read error={error}")
    return validate_layout(parse(payload))


def slot_name(value: str) -> str:
    if value not in ("app0", "app1"):
        raise argparse.ArgumentTypeError("slot must be app0 or app1")
    return value


def _ota_select_crc(sequence: int) -> int:
    # ESP-IDF's bootloader_common_ota_select_crc() hashes ota_seq only and
    # seeds the ROM CRC32 implementation with UINT32_MAX.  The Python
    # equivalent is also used by Espressif's otatool.py.
    return binascii.crc32(struct.pack("<I", sequence), 0xFFFFFFFF) & 0xFFFFFFFF


def ota_sequence(target: str) -> int:
    return 1 if target == "app0" else 2


def build_otadata(original: bytes, target: str, sequence: int | None = None) -> bytes:
    """Return a 0x2000 OTA selector image pointing at one app slot.

    Only the two 32-byte selector records are changed.  All reserved bytes
    from the device backup are preserved, which makes restore evidence useful
    even when a bootloader version adds metadata outside the selector.
    """

    if len(original) != OTADATA_BYTES:
        fail(
            f"otadata_size expected=0x{OTADATA_BYTES:x} "
            f"actual=0x{len(original):x}"
        )
    if target not in ("app0", "app1"):
        fail(f"invalid_target_slot value={target}")
    if sequence is None:
        sequence = ota_sequence(target)
    if sequence <= 0 or sequence >= OTADATA_INVALID_SEQUENCE:
        fail(f"invalid_ota_sequence value={sequence}")

    result = bytearray(original)
    for copy in range(OTADATA_COPIES):
        offset = copy * 0x1000
        # ota_seq, seq_label, ota_state, crc; state 0 means valid.  The
        # selector is intentionally marked valid after the target image has
        # already been read back by the caller.
        body = struct.pack("<I20sI", sequence, b"\0" * 20, 0)
        crc = _ota_select_crc(sequence)
        result[offset : offset + OTADATA_ENTRY_BYTES] = body + struct.pack("<I", crc)
    return bytes(result)


def ota_sequences(payload: bytes) -> tuple[int, int]:
    if len(payload) != OTADATA_BYTES:
        fail("otadata_size_for_read")
    values: list[int] = []
    for copy in range(OTADATA_COPIES):
        offset = copy * 0x1000
        values.append(struct.unpack_from("<I", payload, offset)[0])
    return values[0], values[1]


def validate_otadata(payload: bytes, target: str) -> None:
    expected = ota_sequence(target)
    first, second = ota_sequences(payload)
    if first != expected or second != expected:
        fail(
            f"otadata_target expected={expected} actual={first},{second}"
        )
    for copy in range(OTADATA_COPIES):
        offset = copy * 0x1000
        body = payload[offset : offset + 28]
        stored = struct.unpack_from("<I", payload, offset + 28)[0]
        sequence = struct.unpack_from("<I", body, 0)[0]
        if _ota_select_crc(sequence) != stored:
            fail(f"otadata_crc copy={copy}")


def rescue_plan(
    observed: dict[str, dict[str, int]],
    target: str,
    known_good: str,
    artifact_size: int,
) -> dict[str, object]:
    validate_layout(observed)
    if target == known_good:
        fail("target_and_known_good_must_differ")
    if artifact_size <= 0 or artifact_size > APP_SLOT_BYTES:
        fail(f"artifact_size_exceeds_target_slot size={artifact_size}")
    target_part = observed[target]
    known_part = observed[known_good]
    return {
        "schema": "pokepod.rescue.plan.v1",
        "status": "planned",
        "targetSlot": target,
        "knownGoodSlot": known_good,
        "target": {
            "offset": target_part["offset"],
            "sizeBytes": target_part["size"],
            "writePolicy": "write-candidate-only",
        },
        "knownGood": {
            "offset": known_part["offset"],
            "sizeBytes": known_part["size"],
            "writePolicy": "read-and-preserve",
        },
        "protectedRegions": {
            "partitionTable": {"offset": 0x8000, "sizeBytes": 0x1000},
            "otadata": {"offset": observed["otadata"]["offset"], "sizeBytes": OTADATA_BYTES},
        },
        "artifactSizeBytes": artifact_size,
        "rollbackRule": "known-good-slot-is-never-written",
    }


def _require_sha256(value: object, label: str) -> str:
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-fA-F]{64}", value) is None:
        fail(f"{label}_invalid")
    return value.lower()


def _read_json(path: Path, label: str) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"{label}_invalid error={error}")
    if not isinstance(value, dict):
        fail(f"{label}_invalid")
    return value


def validate_runtime_identity(
    manifest: dict[str, object], identity: dict[str, object], target: str,
    manifest_path: Path | None = None,
) -> dict[str, str]:
    """Require post-rescue identity to match the exact candidate artifact.

    New artifacts carry the ELF digest in ``imageIdentity``.  The legacy
    fields and resource-review evidence remain readable for older artifacts,
    while a present-but-malformed new field is rejected without fallback.
    """

    if identity.get("runningPartition") != target:
        fail(
            f"rescue_running_partition expected={target} "
            f"actual={identity.get('runningPartition')}"
        )
    source_revision = manifest.get("sourceRevision")
    if not isinstance(source_revision, str) or not source_revision:
        fail("rescue_source_revision_manifest_invalid")
    if identity.get("sourceRevision") != source_revision:
        fail("rescue_source_revision_mismatch")

    image_identity = manifest.get("imageIdentity")
    if image_identity is not None:
        if not isinstance(image_identity, dict):
            fail("image_identity_invalid")
        expected_elf = _require_sha256(
            image_identity.get("appElfSha256"), "image_identity_app_elf_sha"
        )
    elif manifest.get("appElfSha256") is not None or manifest.get("elfSha256") is not None:
        expected_elf = _require_sha256(
            manifest.get("appElfSha256") or manifest.get("elfSha256"),
            "legacy_app_elf_sha",
        )
    else:
        review_meta = manifest.get("resourceReview")
        review_ref = review_meta.get("evidenceFile") if isinstance(review_meta, dict) else None
        if not isinstance(review_ref, str) or not review_ref or manifest_path is None:
            fail("rescue_app_elf_sha_missing")
        review_path = (manifest_path.parent / review_ref).resolve()
        review = _read_json(review_path, "resource_review")
        review_elf = review.get("elf")
        expected_elf = _require_sha256(
            review_elf.get("sha256") if isinstance(review_elf, dict) else None,
            "review_app_elf_sha",
        )

    actual_elf = _require_sha256(
        identity.get("appElfSha256") or identity.get("elfSha256"),
        "runtime_app_elf_sha",
    )
    if actual_elf != expected_elf:
        fail("rescue_app_elf_sha_mismatch")
    return {
        "runningPartition": target,
        "sourceRevision": source_revision,
        "appElfSha256": actual_elf,
    }


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def command_layout(args: argparse.Namespace) -> int:
    observed = load_and_validate(args.partition_table)
    write_json(args.output, {
        "schema": "pokepod.rescue.partition-layout.v1",
        "status": "pass",
        "expected": EXPECTED,
        "observed": observed,
    })
    print("PASS rescue_partition_layout")
    return 0


def command_plan(args: argparse.Namespace) -> int:
    observed = load_and_validate(args.partition_table)
    plan = rescue_plan(observed, args.target_slot, args.known_good_slot, args.artifact_size)
    write_json(args.output, plan)
    print(f"PASS rescue_plan target={args.target_slot} known_good={args.known_good_slot}")
    return 0


def command_otadata(args: argparse.Namespace) -> int:
    try:
        original = args.input.read_bytes()
    except OSError as error:
        fail(f"otadata_read error={error}")
    candidate = build_otadata(original, args.target_slot, args.sequence)
    validate_otadata(candidate, args.target_slot)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(candidate)
    print(f"PASS rescue_otadata target={args.target_slot}")
    return 0


def command_verify_otadata(args: argparse.Namespace) -> int:
    try:
        payload = args.input.read_bytes()
    except OSError as error:
        fail(f"otadata_read error={error}")
    validate_otadata(payload, args.target_slot)
    print(f"PASS rescue_otadata_verified target={args.target_slot}")
    return 0


def command_runtime(args: argparse.Namespace) -> int:
    manifest = _read_json(args.manifest, "artifact_manifest")
    identity = _read_json(args.application, "runtime_identity")
    result = validate_runtime_identity(
        manifest, identity, args.target_slot, manifest_path=args.manifest
    )
    print(
        "PASS rescue_runtime_identity "
        f"partition={result['runningPartition']} "
        f"sourceRevision={result['sourceRevision']} "
        f"appElfSha256={result['appElfSha256']}"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    layout = sub.add_parser("layout")
    layout.add_argument("--partition-table", type=Path, required=True)
    layout.add_argument("--output", type=Path, required=True)
    layout.set_defaults(handler=command_layout)

    plan = sub.add_parser("plan")
    plan.add_argument("--partition-table", type=Path, required=True)
    plan.add_argument("--target-slot", type=slot_name, required=True)
    plan.add_argument("--known-good-slot", type=slot_name, required=True)
    plan.add_argument("--artifact-size", type=int, required=True)
    plan.add_argument("--output", type=Path, required=True)
    plan.set_defaults(handler=command_plan)

    otadata = sub.add_parser("otadata")
    otadata.add_argument("--input", type=Path, required=True)
    otadata.add_argument("--target-slot", type=slot_name, required=True)
    otadata.add_argument("--sequence", type=int)
    otadata.add_argument("--output", type=Path, required=True)
    otadata.set_defaults(handler=command_otadata)

    verify = sub.add_parser("verify-otadata")
    verify.add_argument("--input", type=Path, required=True)
    verify.add_argument("--target-slot", type=slot_name, required=True)
    verify.set_defaults(handler=command_verify_otadata)

    runtime = sub.add_parser("runtime")
    runtime.add_argument("--manifest", type=Path, required=True)
    runtime.add_argument("--application", type=Path, required=True)
    runtime.add_argument("--target-slot", type=slot_name, required=True)
    runtime.set_defaults(handler=command_runtime)

    args = parser.parse_args()
    return args.handler(args)


if __name__ == "__main__":
    raise SystemExit(main())
