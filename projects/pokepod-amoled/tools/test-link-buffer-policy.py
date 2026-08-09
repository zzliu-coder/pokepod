#!/usr/bin/env python3
"""Keep Link v2 frame buffers out of scarce ESP32-S3 internal DRAM."""

from pathlib import Path


root = Path(__file__).resolve().parents[1]
firmware = root / "firmware" / "PokePodAmoled"
header = (firmware / "PokePodLinkService.h").read_text(encoding="utf-8")
source = (firmware / "PokePodLinkService.cpp").read_text(encoding="utf-8")

assert "uint8_t *payload_ = nullptr;" in header
assert "uint8_t payload_[kLinkMaxDataBytes]" not in header
assert "heap_caps_calloc(" in source
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in source
assert "std::min<size_t>(kLinkMaxDataBytes, length - sent)" in source
assert "payloadUsed_ < kLinkMaxDataBytes" in source
assert "sizeof(payload_)" not in source

print("PASS test_link_buffer_policy")
