#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "FS.h"
#include "esp_heap_caps.h"

// Drive the exact production repository, metadata codec and storage
// coordinator. This intentionally avoids a test-only scanner model.
#include "../PokePodAmoled/StorageCoordinator.cpp"
#include "../PokePodAmoled/CapsuleTransaction.cpp"
#include "../PokePodAmoled/CapsuleMetadataCodec.cpp"
#define jsonString capsuleLibraryJsonString
#include "../PokePodAmoled/CapsuleLibrary.cpp"
#undef jsonString

using namespace pokepod;

namespace {

struct FixtureRecord {
  std::string id;
  std::string directory;
  std::string folder;
  bool damaged = false;
  bool readOnly = false;
  bool queued = false;
  bool wav = false;
};

std::string uuidFor(size_t index) {
  char value[37];
  std::snprintf(value, sizeof(value), "%08x-1234-4abc-8def-%012zx",
                static_cast<unsigned>(index + 1), index + 1);
  return value;
}

std::string capsuleJson(const std::string &id, size_t index,
                        int schema = 1) {
  char value[512];
  std::snprintf(value, sizeof(value),
                "{\"schemaVersion\":%d,\"id\":\"%s\","
                "\"title\":\"capsule-%zu\","
                "\"createdAt\":\"2026-08-%02zuT00:00:00Z\","
                "\"updatedAt\":\"2026-08-%02zuT00:00:01Z\","
                "\"favorite\":%s,\"revision\":1}",
                schema, id.c_str(), index, index % 28 + 1,
                index % 28 + 1, index % 7 == 0 ? "true" : "false");
  return value;
}

std::string processingJson(const std::string &id, const char *status,
                           const char *audioFile = "audio.wav",
                           int schema = 2) {
  char value[640];
  const char *format = std::strcmp(audioFile, "audio.m4a") == 0
      ? "m4a-aac-lc" : "wav-pcm-s16le";
  std::snprintf(value, sizeof(value),
                "{\"schemaVersion\":%d,\"capsuleId\":\"%s\","
                "\"revision\":1,\"status\":\"%s\","
                "\"audioFile\":\"%s\",\"audioFormat\":\"%s\","
                "\"durationMs\":1200,\"sampleRateHz\":16000,"
                "\"channels\":1,\"bitsPerSample\":16}",
                schema, id.c_str(), status, audioFile, format);
  return value;
}

FixtureRecord seedRecord(size_t index, size_t queuedIndex,
                         const std::shared_ptr<fakefs::State> &filesystem) {
  FixtureRecord record;
  record.id = uuidFor(index);
  std::string root = "/PokeCapsule/Inbox";
  record.folder = "Inbox";
  if (index % 11 == 0) {
    root = "/PokeCapsule/Archive";
    record.folder = "Archive";
  }
  if (index % 17 == 0) {
    root = "/PokeCapsule/.trash";
    record.folder = ".trash";
  }
  if (index % 19 == 0) {
    root = "/PokeCapsule/Projects/Nested";
    record.folder = "Projects/Nested";
  }
  record.directory = root + "/" + record.id;
  record.queued = index == queuedIndex;
  record.wav = index % 2 == 0;

  if (index == 5) {
    filesystem->seed(record.directory + "/capsule.json", "{malformed");
    record.damaged = true;
    record.readOnly = true;
  } else {
    const int schema = index == 7 ? 9 : 1;
    filesystem->seed(record.directory + "/capsule.json",
                     capsuleJson(record.id, index, schema));
    record.readOnly = index == 7;
  }
  if (index != 9) {
    const int schema = index == 7 ? 9 : (index == 3 ? 1 : 2);
    filesystem->seed(
        record.directory + "/processing.json",
        processingJson(record.id, record.queued ? "queued" : "ready",
                       record.wav ? "audio.wav" : "audio.m4a", schema));
  } else {
    record.damaged = true;
    record.readOnly = true;
  }
  filesystem->seed(record.directory + (record.wav ? "/audio.wav"
                                                   : "/audio.m4a"),
                   "fixture-audio");
  filesystem->seed(record.directory + "/raw.txt", "lazy preview text");
  return record;
}

std::vector<FixtureRecord> seedFixture(
    size_t first, size_t count, size_t queuedIndex,
    const std::shared_ptr<fakefs::State> &filesystem) {
  std::vector<FixtureRecord> records;
  records.reserve(count);
  for (size_t index = first; index < first + count; ++index) {
    records.push_back(seedRecord(index, queuedIndex, filesystem));
  }
  return records;
}

void finishScan(CapsuleLibrary &library) {
  while (library.scanState() == CapsuleScanState::running) {
    library.stepScan();
  }
}

void assertHydrated(CapsuleLibrary &library, const FixtureRecord &expected) {
  CapsuleSummary record;
  assert(library.hydrate(expected.id.c_str(), record, true));
  assert(record.id == expected.id.c_str());
  assert(record.folder == expected.folder.c_str());
  assert(record.readOnly == expected.readOnly);
  assert((record.status == CapsuleStatus::damaged) == expected.damaged);
  if (expected.damaged) {
    assert(record.audioFile == (expected.wav ? "audio.wav" : "audio.m4a"));
  } else {
    assert(record.audioFile == (expected.wav ? "audio.wav" : "audio.m4a"));
    assert(record.preview == "lazy preview text");
  }
}

void runProductionLargeFixture() {
  auto state = std::make_shared<fakefs::State>();
  fs::FS filesystem(state);
  Print log;
  CapsuleLibrary library;
  assert(library.begin(filesystem, log));
  assert(library.indexedCount() == 0);

  const auto records = seedFixture(0, 360, 359, state);
  const uint32_t oldRevision = library.revision();
  assert(library.startScan({1, 1024}));
  while (library.scanState() == CapsuleScanState::running) {
    assert(library.indexedCount() == 0);
    assert(library.revision() == oldRevision);
    library.stepScan();
  }
  assert(library.scanState() == CapsuleScanState::completed);
  assert(library.indexedCount() == 360);
  assert(library.scanSlices() > 720);
  assert(library.maximumScanEntriesPerSlice() <= 1);
  assert(library.maximumScanBytesPerSlice() <= 1024);
  assert(library.maximumScanLeaseUs() < 1000000);
  assert(library.maximumScanStepUs() < 1000000);
  assert(library.maximumScanStepUs() >= library.maximumScanLeaseUs());
  assert(!library.indexOverflow());

  const CapsuleSummary *queued = library.nextQueued();
  assert(queued != nullptr);
  assert(queued->id == records.back().id.c_str());
  assert(queued->audioFile == "audio.m4a");
  assert(!queued->readOnly);

  assertHydrated(library, records[2]);
  assertHydrated(library, records[3]);
  CapsuleSummary legacy;
  assert(library.hydrate(records[3].id.c_str(), legacy));
  assert(legacy.processingSchemaVersion == 1);
  assert(legacy.audioFile == "audio.m4a");
  assert(legacy.audioFormat == "m4a-aac-lc");
  assert(legacy.sampleRateHz == 16000);
  assertHydrated(library, records[5]);
  assertHydrated(library, records[7]);
  assertHydrated(library, records[9]);
  assertHydrated(library, records[19]);

  // A Mac-triggered rescan remains atomic: the new capsule is invisible until
  // the repository publishes the completed staging index.
  const FixtureRecord added = seedRecord(360, 360, state);
  assert(library.startScan({1, 1024}));
  while (library.scanState() == CapsuleScanState::running) {
    assert(library.indexedCount() == 360);
    library.stepScan();
  }
  assert(library.scanState() == CapsuleScanState::completed);
  assert(library.indexedCount() == 361);
  CapsuleSummary hydrated;
  assert(library.hydrate(added.id.c_str(), hydrated));
  assert(hydrated.status == CapsuleStatus::queued);
}

void runProductionCancelAndCapacityFailure() {
  auto state = std::make_shared<fakefs::State>();
  fs::FS filesystem(state);
  Print log;
  CapsuleLibrary library;
  const auto original = seedFixture(0, 64, 63, state);
  assert(library.begin(filesystem, log));
  assert(library.indexedCount() == original.size());

  seedFixture(64, 296, 359, state);
  assert(library.startScan({1, 1024}));
  for (size_t slice = 0; slice < 20; ++slice) {
    assert(library.stepScan() == CapsuleScanState::running);
  }
  library.cancelScan();
  finishScan(library);
  assert(library.scanState() == CapsuleScanState::cancelled);
  assert(library.indexedCount() == original.size());
  assert(library.find(original.back().id.c_str()) != nullptr);

  // The capacity contract is fail-closed. A 513th locator fails the staged
  // rebuild and the previously published 64-entry index remains intact.
  seedFixture(360, 153, 512, state);
  assert(library.startScan({1, 1024}));
  finishScan(library);
  assert(library.scanState() == CapsuleScanState::failed);
  assert(library.indexOverflow());
  assert(library.indexedCount() == original.size());
  assert(library.find(original.front().id.c_str()) != nullptr);
}

void runProductionHeapSoak() {
  auto state = std::make_shared<fakefs::State>();
  fs::FS filesystem(state);
  Print log;
  CapsuleLibrary library;
  const auto records = seedFixture(0, 32, 31, state);
  assert(library.begin(filesystem, log));
  const size_t fixedIndexBytes = fake_heap_caps::liveBytes();
  assert(fixedIndexBytes ==
         2 * kCapsuleLocatorCapacity * sizeof(CapsuleLocator));

  CapsuleSummary detail;
  for (size_t cycle = 0; cycle < 10000; ++cycle) {
    assert(library.hydrate(records[cycle % records.size()].id.c_str(), detail,
                           cycle % 3 == 0));
    assert(library.startScan({1, 1024}));
    assert(library.stepScan() == CapsuleScanState::running);
    library.cancelScan();
    finishScan(library);
    assert(library.scanState() == CapsuleScanState::cancelled);
  }
  assert(fake_heap_caps::liveBytes() == fixedIndexBytes);
  assert(library.indexedCount() == records.size());
}

}  // namespace

int main() {
  runProductionLargeFixture();
  runProductionCancelAndCapacityFailure();
  runProductionHeapSoak();
  assert(fake_heap_caps::liveBytes() == 0);
  return 0;
}
