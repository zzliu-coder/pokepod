#!/usr/bin/env python3
"""Verify compressed fixed UI glyphs preserve the accepted 4-bit raster."""

from __future__ import annotations

import hashlib
from pathlib import Path
import re
import struct
import sys


project = Path(__file__).parents[1]
header = (project / "firmware/PokePodAmoled/FixedChineseFont.h").read_text(
    encoding="utf-8"
)

legacy = {
    16: (431, 55_168, "ba2cbb486cb6a387890782907d8af2ebdb993f70cb660a682a8baf347744b06e"),
    20: (431, 86_200, "d82ef4f495ace664379405816af4d260eb36335b24c4438888a8260243db00c9"),
    28: (431, 168_952, "04a2bcd0b2161f7d4fda8f82504871fcc0f242988ce46b06488a2de2344df219"),
    36: (13, 8_424, "9dfb44a63620d74707164be76e350520bbf1135935679fda938336c87a91f02b"),
}


def decode_packbits(encoded: bytes, decoded_bytes: int) -> tuple[bytes, int]:
    decoded = bytearray()
    offset = 0
    packets = 0
    while offset < len(encoded):
        packets += 1
        control = encoded[offset]
        offset += 1
        length = (control & 0x7F) + 1
        assert len(decoded) + length <= decoded_bytes, "decoded overflow"
        if control & 0x80:
            assert offset < len(encoded), "truncated repeat"
            decoded.extend([encoded[offset]] * length)
            offset += 1
        else:
            assert offset + length <= len(encoded), "truncated literal"
            decoded.extend(encoded[offset:offset + length])
            offset += length
    assert len(decoded) == decoded_bytes, "decoded underflow"
    return bytes(decoded), packets


sys.path.insert(0, str(project / "tools"))
from font_packbits import encode_packbits  # noqa: E402
for payload in (
    b"",
    bytes(range(128)),
    bytes(range(129)),
    b"\x00" * 300,
    bytes((index * 37) & 0xFF for index in range(513)),
):
    encoded = encode_packbits(payload)
    if payload:
        decoded, _ = decode_packbits(encoded, len(payload))
        assert decoded == payload
    else:
        assert encoded == b""


compressed_binary_bytes = 0
legacy_binary_bytes = 0
total_packets = 0
maximum_encoded_bytes = 0
for size, (expected_count, expected_raw_bytes, expected_sha) in legacy.items():
    index_body = re.search(
        rf"kFixedGlyphs{size}\[\]\s*=\s*\{{(.*?)\n\}};",
        header,
        re.DOTALL,
    )
    data_body = re.search(
        rf"kFixedGlyphData{size}\[\]\s*=\s*\{{(.*?)\n\}};",
        header,
        re.DOTALL,
    )
    assert index_body is not None and data_body is not None
    entries = [
        tuple(int(value, 16) if field == 0 else int(value)
              for field, value in enumerate(match))
        for match in re.findall(
            r"\{0x([0-9a-fA-F]+),\s*(\d+),\s*(\d+),\s*(\d+),\s*0\}",
            index_body.group(1),
        )
    ]
    data = bytes(
        int(value, 16)
        for value in re.findall(r"0x([0-9a-fA-F]{2})", data_body.group(1))
    )
    assert len(entries) == expected_count
    assert [entry[0] for entry in entries] == sorted(entry[0] for entry in entries)

    digest = hashlib.sha256()
    raw_bytes = 0
    expected_offset = 0
    for codepoint, offset, length, advance in entries:
        assert offset == expected_offset, "compressed glyph data must be contiguous"
        assert 0 < length <= len(data) - offset
        encoded = data[offset:offset + length]
        decoded_bytes = (size * size + 1) // 2
        bitmap, packets = decode_packbits(encoded, decoded_bytes)
        digest.update(struct.pack("<IB", codepoint, advance))
        digest.update(bitmap)
        raw_bytes += len(bitmap)
        expected_offset += length
        total_packets += packets
        maximum_encoded_bytes = max(maximum_encoded_bytes, length)
    assert expected_offset == len(data)
    assert raw_bytes == expected_raw_bytes
    assert digest.hexdigest() == expected_sha, f"{size}px raster changed"

    # The old structs were 4-byte aligned: codepoint + advance + bitmap + pad.
    old_entry_bytes = ((4 + 1 + (size * size + 1) // 2 + 3) // 4) * 4
    legacy_binary_bytes += old_entry_bytes * len(entries)
    compressed_binary_bytes += 12 * len(entries) + len(data)

saving = legacy_binary_bytes - compressed_binary_bytes
assert saving >= 50 * 1024, f"fixed font saving too small: {saving} bytes"
print(
    "PASS test_fixed_font_rle "
    f"(legacy={legacy_binary_bytes}, compressed={compressed_binary_bytes}, "
    f"saving={saving} bytes, packets={total_packets}, "
    f"max_glyph_input={maximum_encoded_bytes} bytes)"
)
