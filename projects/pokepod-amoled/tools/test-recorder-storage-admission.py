#!/usr/bin/env python3
"""Contract for storage-owned recorder admission and bounded media probing."""

from pathlib import Path

root = Path(__file__).parents[1]
firmware = root / "firmware" / "PokePodAmoled"
app = (firmware / "PokePodApp.cpp").read_text(encoding="utf-8")
link = (firmware / "PokePodLinkService.cpp").read_text(encoding="utf-8")
wav_h = (firmware / "WavRecorder.h").read_text(encoding="utf-8")
wav = (firmware / "WavRecorder.cpp").read_text(encoding="utf-8")
source = (firmware / "RecordingCapacitySource.h").read_text(encoding="utf-8")
policy = (firmware / "RecordingAdmissionPolicy.h").read_text(encoding="utf-8")
qualification = (firmware / "RecordingStorageQualification.h").read_text(
    encoding="utf-8")
scenario = (root / "firmware" / "tests" /
            "test_recording_capacity_admission.cpp").read_text(encoding="utf-8")

assert "class RecordingCapacitySource" in source
assert "virtual RecordingSpaceSnapshot query() = 0" in source
assert "virtual uint32_t mountGeneration() const = 0" in source
assert "RecordingCapacitySource &capacitySource" in wav_h
assert "const RecordingSpaceSnapshot &space" not in wav_h
assert "SD_MMC.totalBytes()" not in link
assert "SD_MMC.usedBytes()" not in link
assert app.count("SD_MMC.totalBytes()") == 1
assert app.count("SD_MMC.usedBytes()") == 1
assert "recorder.begin(SD_MMC, recordingCapacitySource" in app

start = wav[wav.index("bool WavRecorder::startStorageSession"):
            wav.index("bool WavRecorder::appendMono16")]
assert start.index("StorageCoordinator::instance().reserve") < start.index(
    "capacitySource_->query()")
assert start.index("StorageCoordinator::instance().acquireIo") < start.index(
    "capacitySource_->query()")
assert start.index("capacitySource_->query()") < start.index("fs_->mkdir")
assert start.index("runStoragePerformanceProbe") < start.index(
    "recording_ = true")
assert "storageReservationTimeoutMs()" in start
assert "storageIoTimeoutMs()" in start
assert "RecorderOperationOwner::linkWifi" in wav
assert "? 0U" in wav
assert "ftruncate(" not in wav
assert "kRecordingProbeBytes" in policy
assert "kRecordingProbeMaximumTailUs" in policy
assert "* 60ULL /" in policy
assert "kRecordingQualificationMaximumAgeUs" in qualification
assert "pendingInvalidations_.fetch_or" in qualification
assert "invalidationEpoch_.fetch_add" in qualification
assert "publishedSequence_" in qualification
assert "RecordingQualificationSnapshot snapshot() const" in qualification
assert "RecordingQualificationInvalidReason::queueHighWater" in wav
assert "RecordingQualificationInvalidReason::shortWrite" in wav
assert "RecordingQualificationInvalidReason::lowSpace" in wav
assert "RecordingQualificationDecision::reuse" in wav
assert "board_.sdMountGeneration()" in app
assert app.count("board.endSdMount();") == 2
assert "kRecordingStorageQueueSafetyMs = 128U * 20U" in policy
assert "storageTooSlow" in wav

for evidence in (
    "capacityUnknown", "capacityInvalid", "insufficientSpace",
    "StorageOwner::capsuleScan", "storageTooSlow", "storageProbeWrite",
    "RecorderOperationOwner::linkWifi", "RecorderStartPollResult::cancelled",
    "source.queryCalls == 1U", "source.queryCalls == 0U",
    "source.queryCalls == 2U", "source.generation = 0",
    "StorageCoordinator::instance().mutationOwner() == StorageOwner::none",
):
    assert evidence in scenario

print("PASS test_recorder_storage_admission")
