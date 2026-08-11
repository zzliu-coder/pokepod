#!/usr/bin/env python3
"""Storage File lifetime, deferred cleanup and SD unmount contract."""

from pathlib import Path


root = Path(__file__).resolve().parents[1]
firmware = root / "firmware/PokePodAmoled"
recorder = (firmware / "WavRecorder.cpp").read_text(encoding="utf-8")
audio = (firmware / "AudioPipeline.cpp").read_text(encoding="utf-8")
tencent = (firmware / "TencentAsr.cpp").read_text(encoding="utf-8")
app = (firmware / "PokePodApp.cpp").read_text(encoding="utf-8")
coordinator = (firmware / "StorageCoordinator.h").read_text(encoding="utf-8")

assert "bool WavRecorder::pollCleanup" in recorder
assert "if (!lease) return false;" in recorder
assert recorder.index("if (!lease) return false;") < recorder.index(
    "terminalState_.fail(cleanupTerminal_"
)
assert "if (recording_ || cleanupPending_ || file_)" in recorder
assert "if (file_) file_.close();" not in recorder

assert "playbackCleanup_.begin" in audio
assert "playbackCleanup_.poll()" in audio
assert "playing_ || playbackCleanup_.pending()" in audio
assert "StorageIoLease closeIo" not in audio

assert "DeferredFileCleanup audioCleanup" in tencent
assert "while (audioCleanup.pending())" in tencent
assert "storageRead, StorageOwner::tencentRead" in tencent

assert "pollDeferredServiceCleanup();" in app
assert app.index("pollDeferredServiceCleanup();") < app.index(
    "if (linkService.receivingBinary())"
)
assert "captureRuntime.pollFinalize" in app
assert "recorder.cleanupPending()" in app
assert "audio.playbackCleanupPending()" in app
assert "StorageCoordinator::instance().idle()" in app
assert app.count("if (board.sdReady()) SD_MMC.end();") == 2
assert "bool readActive() const;" in coordinator
assert "bool idle() const" in coordinator

print("PASS storage_session_cleanup_contract")
