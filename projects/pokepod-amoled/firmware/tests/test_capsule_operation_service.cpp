#include <assert.h>

#include <map>
#include <string>
#include <vector>

#include "../PokePodAmoled/StorageCoordinator.cpp"
#include "../PokePodAmoled/CapsuleBatchJournalStore.cpp"
#include "../PokePodAmoled/CapsuleTransaction.cpp"
#include "../PokePodAmoled/LinkTreeStepper.cpp"
#include "../PokePodAmoled/CapsuleOperationService.cpp"

using namespace pokepod;

namespace {

constexpr const char *kFirst =
    "11111111-1111-4111-8111-111111111111";
constexpr const char *kSecond =
    "22222222-2222-4222-8222-222222222222";

struct QuietPrint final : public Print {};

struct FakeCatalog final : public CapsuleOperationCatalog {
  std::map<std::string, CapsuleOperationSnapshot> records;
  size_t finished = 0;
  bool lastCommitted = false;
  bool lastRemoved = false;

  bool operationSnapshot(const char *id,
                         CapsuleOperationSnapshot &snapshot) const override {
    const auto found = records.find(id == nullptr ? "" : id);
    if (found == records.end()) return false;
    snapshot = found->second;
    return true;
  }

  bool operationCommitted(const char *, const char *, bool) override {
    return true;
  }

  void operationFinished(const char *, size_t, size_t count,
                         bool committed, bool removed) override {
    finished += count;
    lastCommitted = committed;
    lastRemoved = removed;
  }
};

CapsuleOperationSnapshot snapshot(const char *id, const char *directory,
                                  const char *folder, bool archived,
                                  bool trashed) {
  CapsuleOperationSnapshot value;
  strlcpy(value.id, id, sizeof(value.id));
  strlcpy(value.directory, directory, sizeof(value.directory));
  strlcpy(value.folder, folder, sizeof(value.folder));
  value.revision = 1;
  value.archived = archived;
  value.trashed = trashed;
  return value;
}

void seedCapsule(const std::shared_ptr<fakefs::State> &state,
                 const std::string &directory) {
  state->directories.insert(directory);
  state->addParents(directory);
  state->seed(directory + "/capsule.json", "{\"schemaVersion\":2}\n");
  state->seed(directory + "/processing.json", "{\"status\":\"ready\"}\n");
  state->seed(directory + "/audio.wav", "audio");
}

void bootToReady(CapsuleOperationService &service, fs::FS &storage,
                 Print &log, FakeCatalog *catalog = nullptr) {
  assert(service.begin(storage, log));
  if (catalog != nullptr) service.attachCatalog(*catalog);
  size_t polls = 0;
  while (!service.readyForMutation() && !service.mutationCapabilityBlocked() &&
         polls++ < 20000) {
    service.poll(static_cast<uint32_t>(polls));
  }
  assert(polls < 20000);
  assert(!service.mutationCapabilityBlocked());
  assert(service.readyForMutation());
}

CapsuleOperationOutcome run(CapsuleOperationService &service,
                            CapsuleOperationAction action,
                            const std::vector<String> &ids,
                            const String &changedAt = String()) {
  assert(service.submit(action, ids, changedAt));
  CapsuleOperationOutcome outcome;
  size_t polls = 0;
  while (!service.takeOutcome(outcome) && polls++ < 100000) {
    service.poll(static_cast<uint32_t>(polls + 100));
  }
  assert(polls < 100000);
  assert(!service.busy());
  return outcome;
}

void runHappyPath() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  FakeCatalog catalog;
  const std::string inbox = std::string(kCapsuleInbox) + "/" + kFirst;
  const std::string archive = std::string(kCapsuleArchive) + "/" + kFirst;
  const std::string trash = std::string(kCapsuleTrash) + "/" + kFirst;
  seedCapsule(state, inbox);
  catalog.records[kFirst] = snapshot(kFirst, inbox.c_str(), "Inbox", false,
                                     false);

  CapsuleOperationService service;
  bootToReady(service, storage, log, &catalog);
  assert(service.itemCapacity() == 512);
  assert(service.transactionLimit() == 500);
  assert(service.fixedPoolBytes() == 20992);

  CapsuleOperationOutcome outcome = run(
      service, CapsuleOperationAction::archive, {String(kFirst)});
  assert(outcome.committed && outcome.changed == 1);
  assert(state->directories.count(inbox) == 0);
  assert(state->directories.count(archive) == 1);
  assert(state->text(archive + "/archive.json").find("Inbox") !=
         std::string::npos);

  catalog.records[kFirst] = snapshot(kFirst, archive.c_str(), "Archive", true,
                                     false);
  outcome = run(service, CapsuleOperationAction::unarchive, {String(kFirst)});
  assert(outcome.committed);
  assert(state->directories.count(inbox) == 1);
  assert(state->directories.count(archive) == 0);
  assert(state->files.count(inbox + "/archive.json") == 0);

  catalog.records[kFirst] = snapshot(kFirst, inbox.c_str(), "Inbox", false,
                                     false);
  outcome = run(service, CapsuleOperationAction::trash, {String(kFirst)},
                "2026-08-11T08:00:00Z");
  assert(outcome.committed);
  assert(state->directories.count(trash) == 1);
  assert(state->text(trash + "/trash.json").find("trashedAt") !=
         std::string::npos);

  catalog.records[kFirst] = snapshot(kFirst, trash.c_str(), ".trash", false,
                                     true);
  outcome = run(service, CapsuleOperationAction::restore, {String(kFirst)});
  assert(outcome.committed);
  assert(state->directories.count(inbox) == 1);
  assert(state->files.count(inbox + "/trash.json") == 0);
  assert(service.maximumPollBytes() <= 4096);
}

void runPurgePolicyAndLargeDirectory() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  FakeCatalog catalog;
  const std::string trash = std::string(kCapsuleTrash) + "/" + kFirst;
  seedCapsule(state, trash);
  state->seed(trash + "/trash.json",
              "{\"originalFolder\":\"Inbox\"}\n");
  state->seed(trash + "/unknown.bin", "keep me");
  catalog.records[kFirst] = snapshot(kFirst, trash.c_str(), ".trash", false,
                                     true);
  CapsuleOperationService service;
  bootToReady(service, storage, log, &catalog);
  CapsuleOperationOutcome outcome = run(
      service, CapsuleOperationAction::purge, {String(kFirst)});
  assert(!outcome.committed);
  assert(state->directories.count(trash) == 1);
  assert(state->text(trash + "/unknown.bin") == "keep me");

  state->files.erase(trash + "/unknown.bin");
  for (size_t index = 0; index < 800; ++index) {
    // A large supported audio file exercises many <=4 KiB cleanup turns
    // without growing service RAM or recursively deleting in one turn.
    state->files[trash + "/audio.wav"].push_back(
        static_cast<uint8_t>(index));
  }
  outcome = run(service, CapsuleOperationAction::purge, {String(kFirst)});
  assert(outcome.committed && outcome.changed == 1);
  assert(state->directories.count(trash) == 0);
  assert(service.maximumPollBytes() <= 4096);
}

void runBatchRollback() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  FakeCatalog catalog;
  const std::string first = std::string(kCapsuleInbox) + "/" + kFirst;
  const std::string second = std::string(kCapsuleInbox) + "/" + kSecond;
  seedCapsule(state, first);
  seedCapsule(state, second);
  catalog.records[kFirst] = snapshot(kFirst, first.c_str(), "Inbox", false,
                                     false);
  catalog.records[kSecond] = snapshot(kSecond, second.c_str(), "Inbox", false,
                                      false);
  CapsuleOperationService service;
  bootToReady(service, storage, log, &catalog);

  // The metadata transaction and first directory move consume several rename
  // calls. Search all deterministic fault positions until one reaches the
  // second item, then prove the first item is restored by durable rollback.
  bool observedRollback = false;
  for (uint32_t occurrence = 1; occurrence < 40 && !observedRollback;
       ++occurrence) {
    auto attempt = std::make_shared<fakefs::State>(*state);
    fs::FS attemptStorage(attempt);
    CapsuleOperationService attemptService;
    FakeCatalog attemptCatalog = catalog;
    bootToReady(attemptService, attemptStorage, log, &attemptCatalog);
    attempt->fail(fakefs::Operation::rename, occurrence,
                  fakefs::FaultAction::returnFailure);
    CapsuleOperationOutcome outcome = run(
        attemptService, CapsuleOperationAction::archive,
        {String(kFirst), String(kSecond)});
    attempt->clearFault();
    const bool bothOld = attempt->directories.count(first) == 1 &&
        attempt->directories.count(second) == 1;
    if (!outcome.committed && outcome.requested == 2 && bothOld) {
      observedRollback = true;
      assert(!outcome.rollbackFailed);
    }
  }
  assert(observedRollback);
}

void runSubmissionLimit() {
  QuietPrint log;
  fs::FS storage;
  FakeCatalog catalog;
  CapsuleOperationService service;
  bootToReady(service, storage, log, &catalog);
  std::vector<String> oversized;
  oversized.reserve(501);
  for (size_t index = 0; index < 501; ++index) {
    char id[37];
    snprintf(id, sizeof(id), "%08x-1111-4111-8111-%012x",
             static_cast<unsigned>(index + 1),
             static_cast<unsigned>(index + 1));
    oversized.emplace_back(id);
  }
  assert(!service.submit(CapsuleOperationAction::archive, oversized));
  assert(service.readyForMutation());
}

uint32_t countArchiveOperation(fakefs::Operation operation) {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  FakeCatalog catalog;
  const std::string inbox = std::string(kCapsuleInbox) + "/" + kFirst;
  seedCapsule(state, inbox);
  catalog.records[kFirst] = snapshot(kFirst, inbox.c_str(), "Inbox", false,
                                     false);
  CapsuleOperationService service;
  bootToReady(service, storage, log, &catalog);
  state->fail(operation, UINT32_MAX, fakefs::FaultAction::returnFailure);
  const CapsuleOperationOutcome outcome = run(
      service, CapsuleOperationAction::archive, {String(kFirst)});
  assert(outcome.committed);
  return state->fault.seen;
}

void assertArchiveCoherent(const std::shared_ptr<fakefs::State> &state) {
  const std::string inbox = std::string(kCapsuleInbox) + "/" + kFirst;
  const std::string archive = std::string(kCapsuleArchive) + "/" + kFirst;
  const bool oldState = state->directories.count(inbox) == 1 &&
      state->directories.count(archive) == 0 &&
      state->files.count(inbox + "/archive.json") == 0;
  const bool newState = state->directories.count(inbox) == 0 &&
      state->directories.count(archive) == 1 &&
      state->files.count(archive + "/archive.json") == 1;
  assert(oldState || newState);
}

void recoverLocalTwice(const std::shared_ptr<fakefs::State> &state,
                       FakeCatalog &catalog, Print &log) {
  for (size_t reboot = 0; reboot < 2; ++reboot) {
    fs::FS storage(state);
    CapsuleOperationService recovery;
    assert(recovery.begin(storage, log));
    recovery.attachCatalog(catalog);
    size_t polls = 0;
    while (!recovery.readyForMutation() &&
           !recovery.mutationCapabilityBlocked() && polls++ < 20000) {
      recovery.poll(static_cast<uint32_t>(polls));
    }
    assert(polls < 20000);
    assert(recovery.readyForMutation() ||
           recovery.mutationCapabilityBlocked());
    assert(!recovery.sleepBlocker());
    assert(recovery.maximumPollBytes() <= 4096);
  }
  assert(state->openHandles == 0);
}

void runEveryLocalCutpoint() {
  QuietPrint log;
  constexpr fakefs::Operation operations[] = {
      fakefs::Operation::open,
      fakefs::Operation::read,
      fakefs::Operation::write,
      fakefs::Operation::flush,
      fakefs::Operation::rename,
      fakefs::Operation::remove,
      fakefs::Operation::close,
  };
  for (const fakefs::Operation operation : operations) {
    const uint32_t count = countArchiveOperation(operation);
    assert(count > 0);
    for (uint32_t cutpoint = 1; cutpoint <= count; ++cutpoint) {
      for (const fakefs::FaultAction action : {
               fakefs::FaultAction::crashBefore,
               fakefs::FaultAction::crashAfter}) {
        auto state = std::make_shared<fakefs::State>();
        const std::string inbox =
            std::string(kCapsuleInbox) + "/" + kFirst;
        seedCapsule(state, inbox);
        FakeCatalog catalog;
        catalog.records[kFirst] = snapshot(
            kFirst, inbox.c_str(), "Inbox", false, false);
        {
          fs::FS storage(state);
          CapsuleOperationService service;
          bootToReady(service, storage, log, &catalog);
          state->fail(operation, cutpoint, action);
          try {
            assert(service.submit(CapsuleOperationAction::archive,
                                  {String(kFirst)}));
            CapsuleOperationOutcome outcome;
            size_t polls = 0;
            while (!service.takeOutcome(outcome) && polls++ < 100000) {
              service.poll(static_cast<uint32_t>(polls + 100));
            }
          } catch (const fakefs::SimulatedCrash &) {
          }
        }
        state->clearFault();
        recoverLocalTwice(state, catalog, log);
        assertArchiveCoherent(state);
      }
    }
  }
}

void runPermanentCheckpointFailureTerminal() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  FakeCatalog catalog;
  const std::string inbox = std::string(kCapsuleInbox) + "/" + kFirst;
  const std::string archive = std::string(kCapsuleArchive) + "/" + kFirst;
  seedCapsule(state, inbox);
  catalog.records[kFirst] = snapshot(kFirst, inbox.c_str(), "Inbox", false,
                                     false);
  CapsuleOperationService service;
  bootToReady(service, storage, log, &catalog);
  assert(service.submit(CapsuleOperationAction::archive, {String(kFirst)}));
  size_t polls = 0;
  while (state->directories.count(archive) == 0 && polls++ < 20000) {
    service.poll(static_cast<uint32_t>(polls + 100));
  }
  assert(state->directories.count(archive) == 1);
  state->failAlways(fakefs::Operation::write,
                    fakefs::FaultAction::returnFailure);
  CapsuleOperationOutcome outcome;
  while (!service.takeOutcome(outcome) && polls++ < 40000) {
    service.poll(static_cast<uint32_t>(polls + 100));
  }
  state->clearFault();
  assert(polls < 40000);
  assert(outcome.authorityPreserved);
  assert(service.mutationCapabilityBlocked());
  assert(!service.sleepBlocker());
  assert(StorageCoordinator::instance().mutationOwner() == StorageOwner::none);
}

void runInvalidBootAuthorityTerminal() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  FakeCatalog catalog;
  const std::string marker =
      std::string(CapsuleOperationService::kJournalDirectory) + "/" +
      kFirst + ".cbj";
  state->directories.insert(CapsuleOperationService::kJournalDirectory);
  state->addParents(CapsuleOperationService::kJournalDirectory);
  state->seed(marker, "damaged recovery authority");

  CapsuleOperationService service;
  assert(service.begin(storage, log));
  service.attachCatalog(catalog);
  size_t polls = 0;
  while (!service.mutationCapabilityBlocked() && polls++ < 20000) {
    service.poll(static_cast<uint32_t>(polls));
  }
  assert(polls < 20000);
  assert(service.mutationCapabilityBlocked());
  assert(!service.recoveryActive());
  assert(!service.sleepBlocker());
  assert(state->files.count(marker) == 1);
  assert(state->text(marker) == "damaged recovery authority");
  assert(StorageCoordinator::instance().mutationOwner() == StorageOwner::none);
}

}  // namespace

int main() {
  runHappyPath();
  runPurgePolicyAndLargeDirectory();
  runBatchRollback();
  runSubmissionLimit();
  runEveryLocalCutpoint();
  runPermanentCheckpointFailureTerminal();
  runInvalidBootAuthorityTerminal();
  return 0;
}
