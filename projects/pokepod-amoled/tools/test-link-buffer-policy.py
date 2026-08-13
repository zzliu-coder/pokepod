#!/usr/bin/env python3
"""Keep Link v2 frame buffers out of scarce ESP32-S3 internal DRAM."""

from pathlib import Path


root = Path(__file__).resolve().parents[1]
firmware = root / "firmware" / "PokePodAmoled"
header = (firmware / "PokePodLinkService.h").read_text(encoding="utf-8")
source = (firmware / "PokePodLinkService.cpp").read_text(encoding="utf-8")
transport = (firmware / "LinkTransportSession.cpp").read_text(encoding="utf-8")
file_transfer = (firmware / "LinkFileTransfer.cpp").read_text(encoding="utf-8")

assert "uint8_t *payload_ = nullptr;" in header
assert "uint8_t *txFrame_ = nullptr;" in header
assert "uint8_t *pendingControlFrame_ = nullptr;" in header
assert "uint8_t payload_[kLinkMaxDataBytes]" not in header
assert "heap_caps_calloc(" in source
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in source
assert "std::min(capacity, length_ - read_)" in file_transfer
assert "kLinkWriteSliceBytes = 512" in transport
assert "writeAll" not in source + transport
assert "file_.read(" in file_transfer
assert "StorageAccess::read, 0" in file_transfer
assert "payloadUsed_ < kLinkMaxDataBytes" in transport
assert "sizeof(payload_)" not in source + transport

print("PASS test_link_buffer_policy")
