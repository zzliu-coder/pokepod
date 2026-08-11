#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../PokePodAmoled/StorageCoordinator.cpp"
#include "../PokePodAmoled/CapsuleTransaction.cpp"

using namespace pokepod;

namespace {

class QuietPrint final : public Print {};

uint8_t patternByte(uint32_t offset, uint8_t salt) {
  return static_cast<uint8_t>((offset * 37U + (offset >> 8U) + salt) & 0xffU);
}

class PatternSource final : public CapsuleTransactionByteSource {
 public:
  PatternSource(uint32_t length, uint8_t salt)
      : length_(length), salt_(salt) {}

  uint32_t length() const override { return length_; }

  size_t readAt(uint32_t offset, uint8_t *destination,
                size_t maximumBytes) override {
    if (destination == nullptr || offset > length_) return 0;
    const size_t available = length_ - offset;
    const size_t count = std::min(available, maximumBytes);
    for (size_t index = 0; index < count; ++index) {
      destination[index] = patternByte(
          offset + static_cast<uint32_t>(index), salt_);
    }
    return count;
  }

 private:
  uint32_t length_ = 0;
  uint8_t salt_ = 0;
};

bool terminalResult(CapsuleTransactionPollResult result) {
  return result == CapsuleTransactionPollResult::committed ||
      result == CapsuleTransactionPollResult::recovered ||
      result == CapsuleTransactionPollResult::cancelled ||
      result == CapsuleTransactionPollResult::failed ||
      result == CapsuleTransactionPollResult::cleanupBlocked ||
      result == CapsuleTransactionPollResult::recoveryBlocked;
}

CapsuleTransactionPollResult drive(
    CapsuleTransactionRunner &runner,
    const std::shared_ptr<fakefs::State> &state,
    CapsuleTransactionGate *gate = nullptr, uint32_t nowMs = 0,
    uint32_t maximumPolls = 20000) {
  CapsuleTransactionPollResult result = CapsuleTransactionPollResult::idle;
  for (uint32_t poll = 0; poll < maximumPolls; ++poll) {
    const uint32_t operationsBefore = state->operations;
    result = runner.poll(nowMs, gate);
    const uint32_t physicalOperations = state->operations - operationsBefore;
    assert(physicalOperations <= 1);
    assert(runner.lastPollBytes() <= kCapsuleTransactionPollBytes);
    assert(runner.maximumIoBytes() <= kCapsuleTransactionIoBytes);
    if (terminalResult(result)) return result;
  }
  assert(false && "transaction did not reach a deterministic state");
  return result;
}

void assertPattern(const std::shared_ptr<fakefs::State> &state,
                   const char *path, uint32_t length, uint8_t salt) {
  const auto found = state->files.find(path);
  assert(found != state->files.end());
  assert(found->second.size() == length);
  for (uint32_t offset = 0; offset < length; ++offset) {
    assert(found->second[offset] == patternByte(offset, salt));
  }
}

void seedPattern(const std::shared_ptr<fakefs::State> &state,
                 const char *path, uint32_t length, uint8_t salt) {
  std::vector<uint8_t> bytes(length);
  for (uint32_t offset = 0; offset < length; ++offset) {
    bytes[offset] = patternByte(offset, salt);
  }
  state->seedBytes(path, bytes.data(), bytes.size());
}

std::string transactionBaseFor(const char *key, const char *path) {
  uint32_t crc = 0xffffffffU;
  crc = capsuleTransactionCrc32Update(
      crc, reinterpret_cast<const uint8_t *>(key), strlen(key));
  crc = capsuleTransactionCrc32Update(
      crc, reinterpret_cast<const uint8_t *>(path), strlen(path));
  char name[80];
  snprintf(name, sizeof(name),
           "/PokeCapsule/.system/transactions/tx-%08lx",
           static_cast<unsigned long>(~crc));
  return name;
}

void recoverToCompletion(fs::FS &storage, QuietPrint &log,
                         StorageCoordinator &coordinator,
                         const std::shared_ptr<fakefs::State> &state) {
  CapsuleTransactionRunner recovery;
  assert(recovery.begin(storage, log, coordinator));
  assert(recovery.startRecovery());
  assert(drive(recovery, state) == CapsuleTransactionPollResult::recovered);
  CapsuleTransactionRunner second;
  assert(second.begin(storage, log, coordinator));
  assert(second.startRecovery());
  assert(drive(second, state) == CapsuleTransactionPollResult::recovered);
}

void runLargeStreamingCases() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  StorageCoordinator coordinator;

  PatternSource text(1U * 1024U * 1024U, 17);
  const CapsuleTransactionInput textInput{
      "/PokeCapsule/inbox/large/capsule.json", &text, ""};
  CapsuleTransactionRunner textRunner;
  assert(textRunner.begin(storage, log, coordinator));
  assert(textRunner.startCommit("large-text", &textInput, 1,
                                StorageOwner::capsuleTransaction));
  assert(drive(textRunner, state, nullptr, 600001) ==
         CapsuleTransactionPollResult::committed);
  assertPattern(state, textInput.targetPath.c_str(), text.length(), 17);
  assert(textRunner.maximumPollBytes() <= kCapsuleTransactionIoBytes);
  assert(state->maximumWriteBytes <= kCapsuleTransactionIoBytes);

  constexpr uint32_t kThreeMiB = 3U * 1024U * 1024U;
  PatternSource first(kThreeMiB, 29);
  PatternSource second(kThreeMiB, 31);
  const CapsuleTransactionInput pair[2] = {
      {"/PokeCapsule/inbox/pair/raw.txt", &first, ""},
      {"/PokeCapsule/inbox/pair/processing.txt", &second, ""},
  };
  CapsuleTransactionRunner pairRunner;
  assert(pairRunner.begin(storage, log, coordinator));
  assert(pairRunner.startCommit("large-pair", pair, 2,
                                StorageOwner::capsuleTransaction));
  assert(drive(pairRunner, state) == CapsuleTransactionPollResult::committed);
  assertPattern(state, pair[0].targetPath.c_str(), kThreeMiB, 29);
  assertPattern(state, pair[1].targetPath.c_str(), kThreeMiB, 31);

  constexpr uint32_t kFiveMiB = 5U * 1024U * 1024U;
  constexpr char kPrepared[] = "/PokeCapsule/.recording/prepared.wav";
  constexpr char kTarget[] = "/PokeCapsule/inbox/audio/audio.wav";
  seedPattern(state, kPrepared, kFiveMiB, 43);
  CapsuleTransactionRunner prepared;
  assert(prepared.begin(storage, log, coordinator));
  assert(prepared.startPreparedFile("prepared-5m", kPrepared, kTarget,
                                    StorageOwner::recorder));
  assert(drive(prepared, state) == CapsuleTransactionPollResult::committed);
  assertPattern(state, kTarget, kFiveMiB, 43);
  assert(state->files.count(kPrepared) == 0);

  // The runner owns only a 4 KiB transfer window and fixed transaction facts;
  // source payloads remain outside both String and runner storage.
  assert(sizeof(CapsuleTransactionRunner) < 12U * 1024U);
}

void runDeadlineAndReservationCases() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  StorageCoordinator coordinator;
  PatternSource source(1024U * 1024U, 59);
  const CapsuleTransactionInput input{
      "/PokeCapsule/inbox/deadline/capsule.json", &source, ""};

  CapsuleTransactionRunner runner;
  assert(runner.begin(storage, log, coordinator));
  assert(runner.startCommit("deadline", &input, 1,
                            StorageOwner::wifiLink));
  AbsoluteCapsuleTransactionDeadlineGate gate;
  gate.arm(0, 300);
  for (uint32_t index = 0; index < 20; ++index) {
    const CapsuleTransactionPollResult result = runner.poll(299, &gate);
    assert(result == CapsuleTransactionPollResult::progress ||
           result == CapsuleTransactionPollResult::wouldBlock);
  }
  assert(runner.active());
  assert(runner.reservationHeld());

  const CapsuleTransactionPollResult cancelled = drive(
      runner, state, &gate, 300);
  assert(cancelled == CapsuleTransactionPollResult::cancelled);
  assert(!runner.reservationHeld());
  assert(state->openHandles == 0);
  assert(state->files.count(input.targetPath.c_str()) == 0);

  // A null gate is the USB contract: elapsed wall time has no transaction
  // deadline and the same operation still reaches commit after 300 seconds.
  PatternSource usbSource(1024U * 1024U, 61);
  const CapsuleTransactionInput usbInput{
      "/PokeCapsule/inbox/usb/capsule.json", &usbSource, ""};
  CapsuleTransactionRunner usb;
  assert(usb.begin(storage, log, coordinator));
  assert(usb.startCommit("usb-null-gate", &usbInput, 1,
                         StorageOwner::usbLink));
  assert(drive(usb, state, nullptr, 600001) ==
         CapsuleTransactionPollResult::committed);

  PatternSource heldSource(64U * 1024U, 71);
  const CapsuleTransactionInput heldInput{
      "/PokeCapsule/inbox/held/capsule.json", &heldSource, ""};
  CapsuleTransactionRunner held;
  assert(held.begin(storage, log, coordinator));
  assert(held.startCommit("held", &heldInput, 1,
                          StorageOwner::capsuleTransaction));
  for (uint32_t index = 0; index < 20; ++index) {
    assert(!terminalResult(held.poll(0, nullptr)));
  }
  assert(held.reservationHeld());
  assert(!coordinator.reserve(StorageOwner::wifiLink,
                              StorageAccess::mutation));
  assert(drive(held, state) == CapsuleTransactionPollResult::committed);
}

void runShortIoCases() {
  const struct {
    fakefs::Operation operation;
    fakefs::FaultAction action;
  } cases[] = {
      {fakefs::Operation::write, fakefs::FaultAction::shortWrite},
      {fakefs::Operation::flush, fakefs::FaultAction::returnFailure},
      {fakefs::Operation::close, fakefs::FaultAction::returnFailure},
  };
  for (const auto &test : cases) {
    QuietPrint log;
    auto state = std::make_shared<fakefs::State>();
    fs::FS storage(state);
    StorageCoordinator coordinator;
    PatternSource source(64U * 1024U, 83);
    const CapsuleTransactionInput input{
        "/PokeCapsule/inbox/io/capsule.json", &source, ""};
    CapsuleTransactionRunner runner;
    assert(runner.begin(storage, log, coordinator));
    assert(runner.startCommit("io-fault", &input, 1,
                              StorageOwner::capsuleTransaction));
    state->fail(test.operation, 1, test.action);
    const CapsuleTransactionPollResult result = drive(runner, state);
    assert(result == CapsuleTransactionPollResult::failed ||
           result == CapsuleTransactionPollResult::cleanupBlocked);
    state->clearFault();
    if (!runner.reservationHeld()) {
      recoverToCompletion(storage, log, coordinator, state);
    }
  }

  // Short reads are exercised on the prepared-file CRC path.
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  seedPattern(state, "/PokeCapsule/.recording/short.wav", 64U * 1024U, 89);
  fs::FS storage(state);
  StorageCoordinator coordinator;
  CapsuleTransactionRunner runner;
  assert(runner.begin(storage, log, coordinator));
  assert(runner.startPreparedFile(
      "short-read", "/PokeCapsule/.recording/short.wav",
      "/PokeCapsule/inbox/short/audio.wav", StorageOwner::recorder));
  state->fail(fakefs::Operation::read, 1,
              fakefs::FaultAction::shortRead);
  assert(drive(runner, state) == CapsuleTransactionPollResult::failed);
}

void runPermanentFailureRecovery() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  StorageCoordinator coordinator;
  PatternSource source(32U * 1024U, 97);
  const CapsuleTransactionInput input{
      "/PokeCapsule/inbox/permanent/capsule.json", &source, ""};
  CapsuleTransactionRunner runner;
  assert(runner.begin(storage, log, coordinator));
  assert(runner.startCommit("permanent", &input, 1,
                            StorageOwner::capsuleTransaction));

  // Wait until the durable journal has been published, then make every rename
  // fail. Three identical polls produce a deterministic recoverable terminal.
  while (std::string(runner.phaseName()) != "commit") {
    assert(!terminalResult(runner.poll(0, nullptr)));
  }
  state->failAlways(fakefs::Operation::rename,
                    fakefs::FaultAction::returnFailure);
  const CapsuleTransactionPollResult terminal = drive(runner, state);
  assert(terminal == CapsuleTransactionPollResult::recoveryBlocked ||
         terminal == CapsuleTransactionPollResult::cleanupBlocked);
  assert(!runner.reservationHeld());
  assert(state->openHandles == 0);
  state->clearFault();

  // The blocked runner released the global reservation. Unrelated work can
  // begin, and the durable journal remains sufficient for idempotent restart.
  StorageReservation unrelated = coordinator.reserve(
      StorageOwner::wifiLink, StorageAccess::mutation);
  assert(unrelated);
  unrelated.release();
  recoverToCompletion(storage, log, coordinator, state);
  const auto found = state->files.find(input.targetPath.c_str());
  if (found != state->files.end()) {
    assertPattern(state, input.targetPath.c_str(), source.length(), 97);
  }
}

uint32_t operationCount(fakefs::Operation operation) {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  StorageCoordinator coordinator;
  state->seed("/PokeCapsule/inbox/cutpoint/raw.txt", "old-raw");
  state->seed("/PokeCapsule/inbox/cutpoint/processing.json", "old-processing");
  PatternSource raw(16U * 1024U, 101);
  PatternSource processing(16U * 1024U, 102);
  const CapsuleTransactionInput input[2] = {
      {"/PokeCapsule/inbox/cutpoint/raw.txt", &raw, ""},
      {"/PokeCapsule/inbox/cutpoint/processing.json", &processing, ""},
  };
  CapsuleTransactionRunner runner;
  assert(runner.begin(storage, log, coordinator));
  assert(runner.startCommit("cutpoint", input, 2,
                            StorageOwner::capsuleTransaction));
  state->fail(operation, UINT32_MAX, fakefs::FaultAction::returnFailure);
  assert(drive(runner, state) == CapsuleTransactionPollResult::committed);
  return state->fault.seen;
}

void assertCrashPairCoherent(const std::shared_ptr<fakefs::State> &state,
                             const CapsuleTransactionInput *input,
                             uint32_t length) {
  const bool oldPair = state->text(input[0].targetPath.c_str()) == "old-raw" &&
      state->text(input[1].targetPath.c_str()) == "old-processing";
  const auto raw = state->files.find(input[0].targetPath.c_str());
  const auto processing = state->files.find(input[1].targetPath.c_str());
  bool newPair = raw != state->files.end() &&
      processing != state->files.end() && raw->second.size() == length &&
      processing->second.size() == length;
  if (newPair) {
    for (uint32_t offset = 0; offset < length; ++offset) {
      if (raw->second[offset] != patternByte(offset, 103) ||
          processing->second[offset] != patternByte(offset, 104)) {
        newPair = false;
        break;
      }
    }
  }
  assert(oldPair || newPair);
}

void runCrashCutpoints() {
  const fakefs::Operation operations[] = {
      fakefs::Operation::open,
      fakefs::Operation::read,
      fakefs::Operation::write,
      fakefs::Operation::flush,
      fakefs::Operation::rename,
      fakefs::Operation::remove,
      fakefs::Operation::close,
  };
  for (const fakefs::Operation operation : operations) {
    const uint32_t count = operationCount(operation);
    for (uint32_t cutpoint = 1; cutpoint <= count; ++cutpoint) {
      for (const fakefs::FaultAction action : {
               fakefs::FaultAction::crashBefore,
               fakefs::FaultAction::crashAfter}) {
        QuietPrint log;
        auto state = std::make_shared<fakefs::State>();
        state->seed("/PokeCapsule/inbox/crash/raw.txt", "old-raw");
        state->seed("/PokeCapsule/inbox/crash/processing.json",
                    "old-processing");
        PatternSource raw(16U * 1024U, 103);
        PatternSource processing(16U * 1024U, 104);
        const CapsuleTransactionInput input[2] = {
            {"/PokeCapsule/inbox/crash/raw.txt", &raw, ""},
            {"/PokeCapsule/inbox/crash/processing.json", &processing, ""},
        };
        {
          fs::FS storage(state);
          StorageCoordinator coordinator;
          CapsuleTransactionRunner runner;
          assert(runner.begin(storage, log, coordinator));
          assert(runner.startCommit("crash", input, 2,
                                    StorageOwner::capsuleTransaction));
          state->fail(operation, cutpoint, action);
          try {
            drive(runner, state);
          } catch (const fakefs::SimulatedCrash &) {
          }
        }
        state->clearFault();

        // Reboot recovery is idempotent. Retrying the same operation also
        // removes any pre-journal orphan before it publishes a fresh journal.
        fs::FS rebootedStorage(state);
        StorageCoordinator rebootedCoordinator;
        recoverToCompletion(rebootedStorage, log, rebootedCoordinator, state);
        assertCrashPairCoherent(state, input, raw.length());
        CapsuleTransactionRunner retry;
        assert(retry.begin(rebootedStorage, log, rebootedCoordinator));
        assert(retry.startCommit("crash", input, 2,
                                 StorageOwner::capsuleTransaction));
        assert(drive(retry, state) ==
               CapsuleTransactionPollResult::committed);
        assertPattern(state, input[0].targetPath.c_str(), raw.length(), 103);
        assertPattern(state, input[1].targetPath.c_str(),
                      processing.length(), 104);
      }
    }
  }
}

void runPermanentRemoveRecovery() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  StorageCoordinator coordinator;
  PatternSource source(16U * 1024U, 107);
  const CapsuleTransactionInput input{
      "/PokeCapsule/inbox/remove/capsule.json", &source, ""};
  CapsuleTransactionRunner runner;
  assert(runner.begin(storage, log, coordinator));
  assert(runner.startCommit("permanent-remove", &input, 1,
                            StorageOwner::capsuleTransaction));
  while (std::string(runner.phaseName()) != "cleanup") {
    assert(!terminalResult(runner.poll(0, nullptr)));
  }
  state->failAlways(fakefs::Operation::remove,
                    fakefs::FaultAction::returnFailure);
  assert(drive(runner, state) ==
         CapsuleTransactionPollResult::cleanupBlocked);
  assert(!runner.reservationHeld());
  assert(state->openHandles == 0);
  assert(coordinator.reserve(StorageOwner::usbLink,
                             StorageAccess::mutation));
  state->clearFault();
  recoverToCompletion(storage, log, coordinator, state);
  assertPattern(state, input.targetPath.c_str(), source.length(), 107);
}

void runExistingJournalThenNewCommit() {
  enum class OldRecoveryBranch : uint8_t {
    commitNew,
    rollbackOld,
    alreadyCommitted,
  };
  for (const OldRecoveryBranch branch : {
           OldRecoveryBranch::commitNew,
           OldRecoveryBranch::rollbackOld,
           OldRecoveryBranch::alreadyCommitted,
       }) {
    QuietPrint log;
    auto state = std::make_shared<fakefs::State>();
    fs::FS storage(state);
    StorageCoordinator coordinator;
    constexpr char kTarget[] =
        "/PokeCapsule/Inbox/existing/capsule.json";
    constexpr char kOldPrepared[] =
        "/PokeCapsule/.staging/existing/old.json.part";
    constexpr char kNewPrepared[] =
        "/PokeCapsule/.staging/existing/new.json.part";
    state->seed(kTarget, "original");

    PatternSource oldMemory(16U * 1024U, 111);
    seedPattern(state, kOldPrepared, 16U * 1024U, 111);
    const CapsuleTransactionInput oldInput =
        branch == OldRecoveryBranch::rollbackOld
            ? CapsuleTransactionInput{kTarget, nullptr, kOldPrepared}
            : CapsuleTransactionInput{kTarget, &oldMemory, ""};
    {
      CapsuleTransactionRunner old;
      assert(old.begin(storage, log, coordinator));
      assert(old.startCommit("same-key", &oldInput, 1,
                             StorageOwner::capsuleTransaction));
      const char *stopPhase =
          branch == OldRecoveryBranch::rollbackOld ? "publish-prepared" :
          (branch == OldRecoveryBranch::commitNew ? "commit" : "cleanup");
      uint32_t polls = 0;
      while (std::string(old.phaseName()) != stopPhase && polls++ < 10000U) {
        assert(!terminalResult(old.poll(0, nullptr)));
      }
      assert(polls < 10000U);
    }
    assert(coordinator.mutationOwner() == StorageOwner::none);

    seedPattern(state, kNewPrepared, 20U * 1024U, 113);
    const CapsuleTransactionInput newInput{kTarget, nullptr, kNewPrepared};
    CapsuleTransactionRunner current;
    assert(current.begin(storage, log, coordinator));
    assert(current.startCommit("same-key", &newInput, 1,
                               StorageOwner::capsuleTransaction));
    assert(drive(current, state) ==
           CapsuleTransactionPollResult::committed);
    assertPattern(state, kTarget, 20U * 1024U, 113);
    assert(state->files.count(kNewPrepared) == 0);
    assert(!current.reservationHeld());
    assert(state->openHandles == 0);
  }
}

void runForgedJournalTargetRejected() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  constexpr char kForgedJournal[] =
      "/PokeCapsule/.system/transactions/tx-forged.journal";
  state->seed("/foo", "protected");
  StoredCapsuleTransactionJournal journal{};
  journal.magic = kCapsuleTransactionMagic;
  journal.version = kCapsuleTransactionVersion;
  journal.targetCount = 1;
  strcpy(journal.targets[0].path, "/foo");
  journal.targets[0].expectedLength = 3;
  journal.targets[0].expectedCrc32 = capsuleTransactionCrc32(
      reinterpret_cast<const uint8_t *>("new"), 3);
  journal.targets[0].hadOriginal = 1;
  finalizeCapsuleTransactionJournal(journal);
  assert(journal.crc32 == capsuleTransactionCrc32(
      reinterpret_cast<const uint8_t *>(&journal),
      offsetof(StoredCapsuleTransactionJournal, crc32)));
  state->seedBytes(kForgedJournal,
                   reinterpret_cast<const uint8_t *>(&journal),
                   sizeof(journal));
  state->seed(std::string(kForgedJournal) + ".blocked", "older-corrupt");

  fs::FS storage(state);
  StorageCoordinator coordinator;
  CapsuleTransactionRunner recovery;
  assert(recovery.begin(storage, log, coordinator));
  assert(recovery.startRecovery());
  assert(drive(recovery, state) == CapsuleTransactionPollResult::recovered);
  assert(recovery.recoveryQuarantined());
  assert(state->text("/foo") == "protected");
  assert(state->files.count(kForgedJournal) == 0);
  assert(state->text(std::string(kForgedJournal) + ".blocked") ==
         "older-corrupt");
  assert(state->files.count(std::string(kForgedJournal) + ".blocked.1") == 1);
  assert(!recovery.reservationHeld());
  assert(state->openHandles == 0);
}

void runCorruptJournalIsolation() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  StorageCoordinator coordinator;
  PatternSource first(12U * 1024U, 117);
  PatternSource second(12U * 1024U, 119);
  const CapsuleTransactionInput inputs[2] = {
      {"/PokeCapsule/Inbox/isolate-a/raw.txt", &first, ""},
      {"/PokeCapsule/Inbox/isolate-b/raw.txt", &second, ""},
  };
  for (uint8_t index = 0; index < 2; ++index) {
    CapsuleTransactionRunner interrupted;
    assert(interrupted.begin(storage, log, coordinator));
    const char *key = index == 0 ? "isolate-a" : "isolate-b";
    assert(interrupted.startCommit(key, &inputs[index], 1,
                                   StorageOwner::capsuleTransaction));
    uint32_t polls = 0;
    while (std::string(interrupted.phaseName()) != "commit" &&
           polls++ < 10000U) {
      assert(!terminalResult(interrupted.poll(0, nullptr)));
    }
    assert(polls < 10000U);
  }
  constexpr char kCorrupt[] =
      "/PokeCapsule/.system/transactions/tx-00000000.journal";
  state->seed(kCorrupt, "corrupt");

  CapsuleTransactionRunner recovery;
  assert(recovery.begin(storage, log, coordinator));
  assert(recovery.startRecovery());
  assert(drive(recovery, state) == CapsuleTransactionPollResult::recovered);
  assert(recovery.recoveryQuarantined());
  assertPattern(state, inputs[0].targetPath.c_str(), first.length(), 117);
  assertPattern(state, inputs[1].targetPath.c_str(), second.length(), 119);
  assert(state->files.count(kCorrupt) == 0);
  assert(state->files.count(std::string(kCorrupt) + ".blocked") == 1);
  assert(state->openHandles == 0);

  CapsuleTransactionRunner secondPass;
  assert(secondPass.begin(storage, log, coordinator));
  assert(secondPass.startRecovery());
  assert(drive(secondPass, state) ==
         CapsuleTransactionPollResult::recovered);
}

void runCorruptSameBasePreservesSideEvidence() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  StorageCoordinator coordinator;
  constexpr char kKey[] = "corrupt-same-base";
  constexpr char kTarget[] =
      "/PokeCapsule/Inbox/corrupt-same/raw.txt";
  constexpr char kPrepared[] =
      "/PokeCapsule/.staging/corrupt-same/raw.txt.part";
  const std::string base = transactionBaseFor(kKey, kTarget);
  state->seed(kTarget, "old-target");
  state->seed(base + ".journal", "corrupt-journal");
  state->seed(base + ".0.new", "old-staged-evidence");
  state->seed(base + ".0.bak", "old-backup-evidence");
  seedPattern(state, kPrepared, 20U * 1024U, 131);

  const CapsuleTransactionInput input{kTarget, nullptr, kPrepared};
  CapsuleTransactionRunner current;
  assert(current.begin(storage, log, coordinator));
  assert(current.startCommit(kKey, &input, 1,
                             StorageOwner::capsuleTransaction));
  assert(drive(current, state) == CapsuleTransactionPollResult::committed);
  assertPattern(state, kTarget, 20U * 1024U, 131);
  assert(state->text(base + ".journal.blocked") == "corrupt-journal");
  assert(state->text(base + ".journal.blocked.artifact.0.new") ==
         "old-staged-evidence");
  assert(state->text(base + ".journal.blocked.artifact.0.bak") ==
         "old-backup-evidence");
  assert(state->openHandles == 0);
  assert(!current.reservationHeld());
}

void runPublishedJournalReadbackRequired() {
  for (const auto scenario : {
           std::pair<fakefs::Operation, fakefs::FaultAction>{
               fakefs::Operation::write, fakefs::FaultAction::corruptData},
           {fakefs::Operation::flush, fakefs::FaultAction::corruptData},
           {fakefs::Operation::read, fakefs::FaultAction::shortRead},
       }) {
    QuietPrint log;
    auto state = std::make_shared<fakefs::State>();
    fs::FS storage(state);
    StorageCoordinator coordinator;
    constexpr char kTarget[] =
        "/PokeCapsule/Inbox/journal-readback/raw.txt";
    state->seed(kTarget, "old-authoritative-target");
    PatternSource source(16U * 1024U, 139);
    const CapsuleTransactionInput input{kTarget, &source, ""};
    CapsuleTransactionRunner runner;
    assert(runner.begin(storage, log, coordinator));
    assert(runner.startCommit("journal-readback", &input, 1,
                              StorageOwner::capsuleTransaction));
    const char *faultPhase = scenario.first == fakefs::Operation::read
        ? "journal-readback" : "journal-write";
    uint32_t polls = 0;
    while (std::string(runner.phaseName()) != faultPhase && polls++ < 10000U) {
      assert(!terminalResult(runner.poll(0, nullptr)));
    }
    assert(polls < 10000U);
    state->fail(scenario.first, 1, scenario.second);
    const CapsuleTransactionPollResult result = drive(runner, state);
    assert(result == CapsuleTransactionPollResult::failed ||
           result == CapsuleTransactionPollResult::cleanupBlocked);
    assert(state->text(kTarget) == "old-authoritative-target");
    assert(state->openHandles == 0);
    assert(!runner.reservationHeld());
  }
}

}  // namespace

int main() {
  runLargeStreamingCases();
  runDeadlineAndReservationCases();
  runShortIoCases();
  runPermanentFailureRecovery();
  runCrashCutpoints();
  runPermanentRemoveRecovery();
  runExistingJournalThenNewCommit();
  runForgedJournalTargetRejected();
  runCorruptJournalIsolation();
  runCorruptSameBasePreservesSideEvidence();
  runPublishedJournalReadbackRequired();
  return 0;
}
