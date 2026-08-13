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

FixtureRecord seedCustomRecord(
    size_t index, const std::shared_ptr<fakefs::State> &filesystem,
    const std::string &parent = "/PokeCapsule/Projects/Nested") {
  FixtureRecord record;
  record.id = uuidFor(index);
  record.folder = parent.substr(std::string("/PokeCapsule/").size());
  record.directory = parent + "/" + record.id;
  record.wav = index % 2 == 0;
  filesystem->seed(record.directory + "/capsule.json",
                   capsuleJson(record.id, index));
  filesystem->seed(record.directory + "/processing.json",
                   processingJson(record.id, "ready",
                                  record.wav ? "audio.wav" : "audio.m4a"));
  filesystem->seed(record.directory + (record.wav ? "/audio.wav"
                                                   : "/audio.m4a"),
                   "fixture-audio");
  filesystem->seed(record.directory + "/raw.txt", "custom preview text");
  return record;
}

std::vector<FixtureRecord> seedCustomFixture(
    size_t first, size_t count,
    const std::shared_ptr<fakefs::State> &filesystem) {
  std::vector<FixtureRecord> records;
  records.reserve(count);
  for (size_t index = first; index < first + count; ++index) {
    records.push_back(seedCustomRecord(index, filesystem));
  }
  return records;
}

void finishScan(CapsuleLibrary &library) {
  while (library.scanState() == CapsuleScanState::running) {
    library.stepScan();
  }
}

void finishStartup(CapsuleLibrary &library,
                   const std::shared_ptr<fakefs::State> &filesystem) {
  size_t polls = 0;
  bool reachedPublishPhase = false;
  size_t publishPhaseTransitions = 0;
  while (library.startupActive()) {
    assert(library.indexedCount() == 0);
    assert(library.count() == 0);
    assert(library.revision() == 0);
    const uint32_t before = filesystem->operations;
    const uint64_t beforeNext = filesystem->openNextFileCalls;
    const CapsuleLibraryStartupState state = library.pollStartup(
        static_cast<uint32_t>(polls));
    if (state == CapsuleLibraryStartupState::publishingIndex) {
      assert(!reachedPublishPhase);
      reachedPublishPhase = true;
      ++publishPhaseTransitions;
    }
    if (state == CapsuleLibraryStartupState::selectingInterrupted ||
        state == CapsuleLibraryStartupState::openingProcessing ||
        state == CapsuleLibraryStartupState::readingProcessing ||
        state == CapsuleLibraryStartupState::closingProcessing ||
        state == CapsuleLibraryStartupState::preparingRequeue ||
        state == CapsuleLibraryStartupState::startingRequeueCommit ||
        state == CapsuleLibraryStartupState::pollingRequeueCommit ||
        state == CapsuleLibraryStartupState::finishingStartup) {
      // The startup index is assembled before any interrupted-transcription
      // mutation begins. Public readers remain gated until ready below.
      assert(reachedPublishPhase);
    }
    const uint32_t primitives = filesystem->operations - before;
    const uint64_t enumerations =
        filesystem->openNextFileCalls - beforeNext;
    // Directory enumeration and bounded metadata reads are the only scanner
    // slices that may pair a handle close with their one bounded operation.
    assert(static_cast<uint64_t>(primitives) + enumerations <= 1);
    assert(library.startupMaximumIoBytes() <= 4096);
    if (library.startupActive() &&
        state != CapsuleLibraryStartupState::waitingForAuthority) {
      StorageReservation competing = StorageCoordinator::instance().reserve(
          StorageOwner::capsuleTransaction, StorageAccess::mutation, 0);
      assert(!competing);
    }
    assert(++polls < 200000);
  }
  assert(reachedPublishPhase);
  assert(publishPhaseTransitions == 1);
  assert(library.startupReady());
  assert(!library.startupBlocked());
  assert(filesystem->openHandles == 0);
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
  finishStartup(library, state);
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
  finishStartup(library, state);
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
  // A request arriving during the doomed generation remains sticky. The old
  // index remains visible, and pollScan() starts the requested follow-up only
  // on a later turn.
  assert(library.requestScan({1, 512}));
  finishScan(library);
  assert(library.scanState() == CapsuleScanState::failed);
  assert(library.scanRequested());
  assert(library.indexOverflow());
  assert(library.indexedCount() == original.size());
  assert(library.find(original.front().id.c_str()) != nullptr);
  assert(library.pollScan() == CapsuleScanState::running);
  assert(!library.scanRequested());
  assert(library.scanSlices() == 0);
  library.cancelScan();
  assert(library.pollScan() == CapsuleScanState::cancelled);
  assert(library.indexedCount() == original.size());
}

void runProductionCustomPathIndex() {
  auto state = std::make_shared<fakefs::State>();
  fs::FS filesystem(state);
  Print log;
  CapsuleLibrary library;
  assert(library.begin(filesystem, log));
  finishStartup(library, state);
  const auto records = seedCustomFixture(0, 360, state);
  assert(library.startScan({1, 1024}));
  finishScan(library);
  assert(library.scanState() == CapsuleScanState::completed);
  assert(library.indexedCount() == records.size());
  assert(library.customPathBytes() > records.size() * 64);
  assert(library.customPathBytes() <= library.customPathCapacity());

  // Hydrating 360 custom records churns the 12-entry detail cache repeatedly.
  // Every directory was captured in the published PSRAM path generation, so
  // no post-publish tree enumeration is permitted.
  const uint64_t enumerationsAfterPublish = state->openNextFileCalls;
  for (const FixtureRecord &record : records) {
    CapsuleSummary hydrated;
    assert(library.hydrate(record.id.c_str(), hydrated, true));
    assert(hydrated.directory == record.directory.c_str());
    assert(hydrated.folder == "Projects/Nested");
  }
  assert(state->openNextFileCalls == enumerationsAfterPublish);

  // Incremental metadata and move/restore operations compact into the spare
  // locator/path generation. They never fall back to a recursive custom-path
  // lookup and preserve the original folder semantics.
  const FixtureRecord &changed = records[123];
  assert(library.toggleFavorite(changed.id.c_str()));
  assert(state->openNextFileCalls == enumerationsAfterPublish);
  assert(library.archive(changed.id.c_str()));
  assert(state->openNextFileCalls == enumerationsAfterPublish);
  assert(library.unarchive(changed.id.c_str()));
  assert(state->openNextFileCalls == enumerationsAfterPublish);
  assert(library.trash(changed.id.c_str(), "2026-08-11T12:00:00Z"));
  assert(state->openNextFileCalls == enumerationsAfterPublish);
  assert(library.restore(changed.id.c_str()));
  assert(state->openNextFileCalls == enumerationsAfterPublish);
  CapsuleSummary restored;
  assert(library.hydrate(changed.id.c_str(), restored));
  assert(restored.folder == "Projects/Nested");
  assert(restored.directory == changed.directory.c_str());
  assert(state->openNextFileCalls == enumerationsAfterPublish);

  // A cancelled rebuild cannot expose its staged path generation.
  const size_t oldCount = library.indexedCount();
  const size_t oldPathBytes = library.customPathBytes();
  seedCustomRecord(360, state);
  assert(library.startScan({1, 1024}));
  for (size_t slice = 0; slice < 40; ++slice) {
    assert(library.stepScan() == CapsuleScanState::running);
  }
  library.cancelScan();
  finishScan(library);
  assert(library.scanState() == CapsuleScanState::cancelled);
  assert(library.indexedCount() == oldCount);
  assert(library.customPathBytes() == oldPathBytes);
  assert(library.hydrate(changed.id.c_str(), restored));
  assert(restored.directory == changed.directory.c_str());
}

void runProductionCustomPathFailClosed() {
  auto state = std::make_shared<fakefs::State>();
  fs::FS filesystem(state);
  Print log;
  const FixtureRecord original = seedCustomRecord(0, state);
  CapsuleLibrary library;
  assert(library.begin(filesystem, log));
  finishStartup(library, state);
  const size_t oldPathBytes = library.customPathBytes();

  // This path exceeds the existing 255-byte storage contract. The complete
  // staged generation fails closed and the previously published locator/path
  // pair remains readable.
  const std::string longParent =
      "/PokeCapsule/" + std::string(220, 'x') + "/Nested";
  seedCustomRecord(1, state, longParent);
  assert(library.startScan({1, 1024}));
  finishScan(library);
  assert(library.scanState() == CapsuleScanState::failed);
  assert(library.indexedCount() == 1);
  assert(library.customPathBytes() == oldPathBytes);
  CapsuleSummary hydrated;
  assert(library.hydrate(original.id.c_str(), hydrated));
  assert(hydrated.directory == original.directory.c_str());
}

void runProductionQueuedRescanGate() {
  auto state = std::make_shared<fakefs::State>();
  fs::FS filesystem(state);
  Print log;
  CapsuleLibrary library;
  assert(library.begin(filesystem, log));
  finishStartup(library, state);
  const uint32_t bootScans = library.fullScanCount();
  seedFixture(0, 12, 11, state);

  // Link command completion requests the scan while still owning its mutation
  // reservation. Acceptance is independent of immediate start availability.
  StorageReservation mutation = StorageCoordinator::instance().reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, 0);
  assert(mutation);
  assert(library.requestScan({1, 1024}));
  assert(library.pollScan() != CapsuleScanState::running);
  assert(library.scanRequested());
  mutation.release();

  // Starting consumes this turn without consuming a filesystem slice.
  assert(library.pollScan() == CapsuleScanState::running);
  assert(library.scanSlices() == 0);
  assert(!library.scanRequested());

  // Recorder and Link may both request while the first generation is active.
  // They coalesce into exactly one additional full rebuild and are not lost.
  assert(library.requestScan({1, 1024}));
  assert(library.requestScan({1, 512}));
  assert(library.scanRequested());
  while (library.scanActive()) library.pollScan();
  assert(library.scanState() == CapsuleScanState::completed);
  assert(library.fullScanCount() == bootScans + 1);
  assert(library.scanRequested());

  // The completed turn did not also start the pending generation.
  assert(library.pollScan() == CapsuleScanState::running);
  assert(library.scanSlices() == 0);
  assert(!library.scanRequested());
  while (library.scanActive()) library.pollScan();
  assert(library.fullScanCount() == bootScans + 2);
  assert(library.indexedCount() == 12);
}

void runProductionHeapSoak() {
  auto state = std::make_shared<fakefs::State>();
  fs::FS filesystem(state);
  Print log;
  CapsuleLibrary library;
  const auto records = seedFixture(0, 32, 31, state);
  assert(library.begin(filesystem, log));
  finishStartup(library, state);
  const size_t fixedIndexBytes = fake_heap_caps::liveBytes();
  assert(fixedIndexBytes ==
         2 * kCapsuleLocatorCapacity * sizeof(CapsuleLocator) +
         2 * kCapsuleCustomPathPoolBytes);

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

void runProductionCooperativeStartup() {
  auto state = std::make_shared<fakefs::State>();
  fs::FS filesystem(state);
  Print log;
  const auto records = seedFixture(0, kCapsuleLocatorCapacity,
                                   kCapsuleLocatorCapacity - 1, state);
  const size_t interrupted[] = {10, 173, 511};
  for (const size_t index : interrupted) {
    state->seed(records[index].directory + "/processing.json",
                processingJson(records[index].id, "transcribing",
                               records[index].wav ? "audio.wav" : "audio.m4a"));
  }
  // Unknown schemas remain immutable even if their status resembles an
  // interrupted local job.
  const std::string unknown = processingJson(
      records[7].id, "transcribing",
      records[7].wav ? "audio.wav" : "audio.m4a", 9);
  state->seed(records[7].directory + "/processing.json", unknown);
  state->seed("/PokeCapsule/.system/transactions/bad-one.journal", "bad");
  state->seed("/PokeCapsule/.system/transactions/bad-two.journal", "broken");

  CapsuleLibrary library;
  assert(library.begin(filesystem, log));
  assert(library.startupActive());
  assert(library.indexedCount() == 0);
  assert(library.count() == 0);
  assert(library.nextQueued() == nullptr);
  finishStartup(library, state);

  assert(library.indexedCount() == kCapsuleLocatorCapacity);
  assert(library.startupPolls() > kCapsuleLocatorCapacity);
  assert(library.startupMaximumIoBytes() <= 4096);
  for (const size_t index : interrupted) {
    CapsuleSummary record;
    assert(library.hydrate(records[index].id.c_str(), record));
    assert(record.status == CapsuleStatus::queued);
  }
  CapsuleSummary readOnly;
  assert(library.hydrate(records[7].id.c_str(), readOnly));
  assert(readOnly.readOnly);
  assert(state->text(records[7].directory + "/processing.json") == unknown);
  assert(!state->has("/PokeCapsule/.system/transactions/bad-one.journal"));
  assert(!state->has("/PokeCapsule/.system/transactions/bad-two.journal"));
  assert(state->has(
      "/PokeCapsule/.system/transactions/bad-one.journal.blocked"));
  assert(state->has(
      "/PokeCapsule/.system/transactions/bad-two.journal.blocked"));
}

}  // namespace

int main() {
  runProductionLargeFixture();
  runProductionCancelAndCapacityFailure();
  runProductionCustomPathIndex();
  runProductionCustomPathFailClosed();
  runProductionQueuedRescanGate();
  runProductionHeapSoak();
  runProductionCooperativeStartup();
  assert(fake_heap_caps::liveBytes() == 0);
  return 0;
}
