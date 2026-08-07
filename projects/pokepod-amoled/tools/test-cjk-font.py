#!/usr/bin/env python3
"""Validate the shipped SD font container and antialiasing payload."""

from pathlib import Path
import struct


font_path = Path(__file__).parents[1] / "assets" / "cjk20.a4"
payload = font_path.read_bytes()
magic, width, height, bpp, _, _, count, entry_size = struct.unpack(
    "<4sHHBBHII", payload[:20]
)

assert magic == b"PKF2"
assert (width, height, bpp) == (20, 20, 4)
assert entry_size == 208
assert len(payload) == 20 + count * entry_size

codepoints = []
alpha_values = set()
for index in range(count):
    offset = 20 + index * entry_size
    point, advance = struct.unpack("<IB3x", payload[offset:offset + 8])
    codepoints.append(point)
    assert 1 <= advance <= 20
    for packed in payload[offset + 8:offset + entry_size]:
        alpha_values.add(packed >> 4)
        alpha_values.add(packed & 0x0F)

assert codepoints == sorted(set(codepoints))
assert ord("A") in codepoints
assert ord("语") in codepoints
assert ord("龘") in codepoints
assert 0 in alpha_values and 15 in alpha_values
assert any(0 < alpha < 15 for alpha in alpha_values)
print(f"PASS test_cjk_font ({count} glyphs, A4 grayscale)")
