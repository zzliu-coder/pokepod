#!/usr/bin/env python3
"""Pure-Python bounded PackBits encoder shared by build and source tests."""

from __future__ import annotations


def encode_packbits(data: bytes) -> bytes:
    """Encode bytes as bounded PackBits-style literal/repeat packets.

    A control byte with bit 7 clear is followed by 1..128 literal bytes.
    A control byte with bit 7 set is followed by one byte repeated 1..128
    times. Runs shorter than three bytes stay literal.
    """
    encoded = bytearray()
    offset = 0
    while offset < len(data):
        run = 1
        while (
            offset + run < len(data)
            and data[offset + run] == data[offset]
            and run < 128
        ):
            run += 1
        if run >= 3:
            encoded.extend((0x80 | (run - 1), data[offset]))
            offset += run
            continue

        literal_start = offset
        offset += run
        while offset < len(data) and offset - literal_start < 128:
            run = 1
            while (
                offset + run < len(data)
                and data[offset + run] == data[offset]
                and run < 128
            ):
                run += 1
            if run >= 3 or offset + run - literal_start > 128:
                break
            offset += run
        literal = data[literal_start:offset]
        encoded.append(len(literal) - 1)
        encoded.extend(literal)
    return bytes(encoded)
