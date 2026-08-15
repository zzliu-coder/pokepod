#!/usr/bin/env python3
"""Behavior tests for the fail-closed ROM rescue layout model."""

from __future__ import annotations

import importlib.util
import hashlib
from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "validate-rescue-partitions.py"
spec = importlib.util.spec_from_file_location("rescue_layout", MODULE_PATH)
assert spec and spec.loader
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)


def entry(part_type: int, subtype: int, offset: int, size: int, label: str) -> bytes:
    raw = label.encode("ascii")[:15] + b"\0"
    return struct.pack(
        "<HBBII16sI",
        layout.PARTITION_MAGIC,
        part_type,
        subtype,
        offset,
        size,
        raw.ljust(16, b"\0"),
        0,
    )


def table(*, app1_offset: int = 0x310000, duplicate: bool = False) -> bytes:
    entries = [
        entry(0x01, 0x02, 0x9000, 0x5000, "nvs"),
        entry(0x01, 0x00, 0xE000, 0x2000, "otadata"),
        entry(0x00, 0x10, 0x10000, 0x300000, "app0"),
        entry(0x00, 0x11, app1_offset, 0x300000, "app1"),
    ]
    if duplicate:
        entries.append(entry(0x01, 0x02, 0x5000, 0x1000, "nvs"))
    return b"".join(entries) + b"\xff" * (layout.PARTITION_TABLE_BYTES - len(b"".join(entries)))


def table_with_md5() -> bytes:
    entries = table()[: 4 * layout.PARTITION_TABLE_ENTRY_BYTES]
    record = b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(entries).digest()
    payload = entries + record
    return payload + b"\xff" * (layout.PARTITION_TABLE_BYTES - len(payload))


def expect_failure(callback, fragment: str) -> None:
    try:
        callback()
    except SystemExit as error:
        assert fragment in str(error), str(error)
    else:
        raise AssertionError(f"expected failure containing {fragment!r}")


def main() -> int:
    observed = layout.parse(table())
    layout.validate_layout(observed)
    assert observed["app0"]["offset"] == 0x10000
    assert observed["app1"]["offset"] == 0x310000
    layout.validate_layout(layout.parse(table_with_md5()))
    bad_md5 = bytearray(table_with_md5())
    bad_md5[4 * layout.PARTITION_TABLE_ENTRY_BYTES + 16] ^= 0x01
    expect_failure(lambda: layout.parse(bytes(bad_md5)), "md5_mismatch")

    expect_failure(
        lambda: layout.validate_layout(layout.parse(table(app1_offset=0x320000))),
        "app1.offset",
    )
    expect_failure(lambda: layout.parse(table(app1_offset=0x300000)), "overlap")
    expect_failure(lambda: layout.validate_layout(layout.parse(table(duplicate=True))), "duplicate_label")
    expect_failure(lambda: layout.parse(table()[:-1]), "partition_table_size")

    bad_magic = bytearray(table())
    bad_magic[0:2] = b"\x51\xaa"
    expect_failure(lambda: layout.parse(bytes(bad_magic)), "invalid_magic")

    plan = layout.rescue_plan(observed, "app1", "app0", 0x180000)
    assert plan["targetSlot"] == "app1"
    assert plan["knownGoodSlot"] == "app0"
    assert plan["target"]["offset"] == 0x310000
    assert plan["knownGood"]["writePolicy"] == "read-and-preserve"
    expect_failure(
        lambda: layout.rescue_plan(observed, "app0", "app0", 0x180000),
        "target_and_known_good_must_differ",
    )
    expect_failure(
        lambda: layout.rescue_plan(observed, "app1", "app0", 0x300001),
        "artifact_size_exceeds_target_slot",
    )

    original = bytes([0xA5]) * layout.OTADATA_BYTES
    candidate = layout.build_otadata(original, "app1")
    assert len(candidate) == layout.OTADATA_BYTES
    assert candidate[0x20:0x1000] == original[0x20:0x1000]
    assert candidate[0x1020:] == original[0x1020:]
    layout.validate_otadata(candidate, "app1")
    assert layout.ota_sequences(candidate) == (2, 2)
    expect_failure(lambda: layout.validate_otadata(candidate, "app0"), "otadata_target")
    corrupted = bytearray(candidate)
    corrupted[0x1C] ^= 0x01
    expect_failure(lambda: layout.validate_otadata(bytes(corrupted), "app1"), "otadata_crc")

    print("PASS rescue_partition_layout_behavior")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
