#include <assert.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../PokePodAmoled/StorageCoordinator.cpp"
#include "../PokePodAmoled/LinkFileTransfer.cpp"

using namespace pokepod;

namespace {

struct QueuedFrame {
  LinkFrameType type = LinkFrameType::responseJson;
  uint16_t flags = 0;
  uint32_t requestId = 0;
  std::vector<uint8_t> payload;
  LinkFileTransferFrameCompletion completion =
      LinkFileTransferFrameCompletion::none;
  LinkOperationFrameRole role = LinkOperationFrameRole::progress;
  bool completionEligible = false;
};

class FakeHost final : public LinkFileTransferHost {
 public:
  bool linkFileTransferPermitted() const override { return permitted; }
  bool linkFileTransmitIdle() const override { return transmitIdle; }
  void linkFileCancelForDeadline(uint32_t requestId) override {
    deadlineCancelled = requestId;
  }
  void linkFileSendBusy(uint32_t requestId) override { busy = requestId; }
  void linkFileSendError(uint32_t requestId, const char *message) override {
    error = requestId;
    errorMessage = message == nullptr ? "" : message;
  }
  bool linkFileQueueFrame(
      LinkFrameType type, uint16_t flags, uint32_t requestId,
      const uint8_t *payload, size_t size,
      LinkFileTransferFrameCompletion completion,
      LinkOperationFrameRole role, bool completionEligible) override {
    if (failQueue) return false;
    QueuedFrame frame;
    frame.type = type;
    frame.flags = flags;
    frame.requestId = requestId;
    frame.payload.assign(payload, payload + size);
    frame.completion = completion;
    frame.role = role;
    frame.completionEligible = completionEligible;
    frames.push_back(frame);
    transmitIdle = false;
    return true;
  }
  void linkFileDisconnectTransport() override { ++disconnects; }
  void linkFileClaimResources(uint32_t requestId) override {
    claimedRequest = requestId;
    resources = true;
  }
  void linkFileReleaseResources() override { resources = false; }
  void linkFileAdvanceSettlement() override { ++settlements; }
  void linkFileCancelTransmitFrames() override {
    ++cancelTransmit;
    transmitIdle = true;
  }
  fs::FS *linkFileSystem() override { return &fs; }
  StorageOwner linkFileStorageOwner() const override { return owner; }
  uint32_t linkFileStorageIoTimeout() const override { return 0; }
  uint8_t *linkFilePayloadBuffer() override { return buffer; }
  size_t linkFilePayloadCapacity() const override { return capacity; }
  void linkFileResultFetched(const char *transactionId,
                             bool fullySent) override {
    fetched = fullySent ? (transactionId == nullptr ? "" : transactionId) : "";
  }

  void drainLast(LinkFileTransfer &transfer) {
    assert(!frames.empty());
    const auto completion = frames.back().completion;
    transmitIdle = true;
    transfer.onFrameSent(completion);
  }

  fs::FS fs;
  StorageOwner owner = StorageOwner::usbLink;
  uint8_t buffer[8] = {};
  size_t capacity = 3;
  bool permitted = true;
  bool transmitIdle = true;
  bool failQueue = false;
  bool resources = false;
  uint32_t deadlineCancelled = 0;
  uint32_t busy = 0;
  uint32_t error = 0;
  uint32_t claimedRequest = 0;
  unsigned disconnects = 0;
  unsigned settlements = 0;
  unsigned cancelTransmit = 0;
  std::string errorMessage;
  std::string fetched;
  std::vector<QueuedFrame> frames;
};

void testStreamsOneOwnerAndCompletesAfterCleanup() {
  FakeHost host;
  host.fs.state()->seed("/audio.wav", "abcdefg");
  LinkFileTransfer transfer;
  transfer.bind(host);
  static constexpr const char *kTransaction =
      "11111111-1111-4111-8111-111111111111";

  assert(transfer.send(7, "/audio.wav", kTransaction));
  assert(transfer.active());
  assert(host.resources);
  assert(host.claimedRequest == 7);
  assert(host.frames.size() == 1);
  assert(host.frames[0].type == LinkFrameType::responseJson);
  assert(std::string(host.frames[0].payload.begin(),
                     host.frames[0].payload.end()).find(
                         "\"binaryLength\":7") != std::string::npos);
  assert(host.frames[0].completion ==
         LinkFileTransferFrameCompletion::response);
  assert(!host.frames[0].completionEligible);

  host.drainLast(transfer);
  assert(transfer.dataReady());
  transfer.advance();
  assert(host.frames.size() == 2);
  assert(std::string(host.frames[1].payload.begin(),
                     host.frames[1].payload.end()) == "abc");
  assert(host.frames[1].completion == LinkFileTransferFrameCompletion::data);
  host.drainLast(transfer);

  transfer.advance();
  assert(host.frames.size() == 3);
  assert(std::string(host.frames[2].payload.begin(),
                     host.frames[2].payload.end()) == "def");
  host.drainLast(transfer);

  transfer.advance();
  assert(host.frames.size() == 4);
  assert(std::string(host.frames[3].payload.begin(),
                     host.frames[3].payload.end()) == "g");
  assert(host.frames[3].completion == LinkFileTransferFrameCompletion::final);
  assert(host.frames[3].role == LinkOperationFrameRole::terminal);
  assert(host.frames[3].completionEligible);
  host.drainLast(transfer);

  assert(transfer.quiesced());
  assert(!host.resources);
  assert(host.fetched == kTransaction);
  assert(host.settlements == 1);
  assert(host.fs.state()->openHandles == 0);
  StorageReservation next = StorageCoordinator::instance().reserve(
      StorageOwner::wifiLink, StorageAccess::read);
  assert(next);
}

void testQueueFailureNeverPublishesResult() {
  FakeHost host;
  host.fs.state()->seed("/result.json", "result");
  host.failQueue = true;
  LinkFileTransfer transfer;
  transfer.bind(host);

  assert(!transfer.send(8, "/result.json",
                        "22222222-2222-4222-8222-222222222222"));
  assert(transfer.quiesced());
  assert(host.fetched.empty());
  assert(!host.resources);
  assert(host.settlements == 1);
  assert(host.fs.state()->openHandles == 0);
}

void testEmptyFileUsesTerminalResponseAndCompletesAfterCleanup() {
  FakeHost host;
  host.fs.state()->seed("/empty.wav", "");
  LinkFileTransfer transfer;
  transfer.bind(host);
  static constexpr const char *kTransaction =
      "33333333-3333-4333-8333-333333333333";

  assert(transfer.send(12, "/empty.wav", kTransaction));
  assert(host.frames.size() == 1);
  assert(host.frames[0].role == LinkOperationFrameRole::terminal);
  assert(host.frames[0].completionEligible);
  assert(host.fetched.empty());

  host.drainLast(transfer);
  assert(transfer.quiesced());
  assert(host.fetched == kTransaction);
  assert(host.settlements == 1);
  assert(host.fs.state()->openHandles == 0);
}

void testBusyPhysicalIoRetriesWithoutDuplicatingBytes() {
  FakeHost host;
  host.fs.state()->seed("/retry.wav", "abcd");
  LinkFileTransfer transfer;
  transfer.bind(host);
  assert(transfer.send(13, "/retry.wav"));
  host.drainLast(transfer);

  std::atomic<bool> leaseReady{false};
  std::atomic<bool> releaseLease{false};
  std::thread other([&]() {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        StorageOwner::fontRead, StorageAccess::read, 50);
    assert(lease);
    leaseReady.store(true);
    while (!releaseLease.load()) std::this_thread::yield();
  });
  while (!leaseReady.load()) std::this_thread::yield();

  transfer.advance();
  assert(host.frames.size() == 1);
  assert(host.disconnects == 0);

  releaseLease.store(true);
  other.join();
  transfer.advance();
  assert(host.frames.size() == 2);
  assert(std::string(host.frames[1].payload.begin(),
                     host.frames[1].payload.end()) == "abc");
  host.drainLast(transfer);
  transfer.advance();
  assert(host.frames.size() == 3);
  assert(std::string(host.frames[2].payload.begin(),
                     host.frames[2].payload.end()) == "d");
  host.drainLast(transfer);
  assert(transfer.quiesced());
}

void testFinalFrameDisconnectBeforeCleanupNeverPublishesResult() {
  FakeHost host;
  host.capacity = sizeof(host.buffer);
  host.fs.state()->seed("/result.json", "result");
  LinkFileTransfer transfer;
  transfer.bind(host);
  assert(transfer.send(14, "/result.json",
                       "44444444-4444-4444-8444-444444444444"));
  host.drainLast(transfer);
  transfer.advance();
  assert(host.frames.size() == 2);
  assert(host.frames.back().completion ==
         LinkFileTransferFrameCompletion::final);

  std::atomic<bool> leaseReady{false};
  std::atomic<bool> releaseLease{false};
  std::thread other([&]() {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        StorageOwner::fontRead, StorageAccess::read, 50);
    assert(lease);
    leaseReady.store(true);
    while (!releaseLease.load()) std::this_thread::yield();
  });
  while (!leaseReady.load()) std::this_thread::yield();

  host.drainLast(transfer);
  assert(transfer.cleanupPending());
  assert(host.fetched.empty());
  transfer.abort();
  assert(host.fetched.empty());

  releaseLease.store(true);
  other.join();
  assert(transfer.pollCleanup());
  assert(transfer.quiesced());
  assert(host.fetched.empty());
  assert(!host.resources);
  assert(host.fs.state()->openHandles == 0);
}

void testDataQueueFailureDisconnectsAndReleasesEverything() {
  FakeHost host;
  host.fs.state()->seed("/queue.wav", "abcd");
  LinkFileTransfer transfer;
  transfer.bind(host);
  assert(transfer.send(15, "/queue.wav",
                       "55555555-5555-4555-8555-555555555555"));
  host.drainLast(transfer);
  host.failQueue = true;
  transfer.advance();

  assert(host.disconnects == 1);
  assert(transfer.quiesced());
  assert(host.fetched.empty());
  assert(!host.resources);
  assert(host.settlements == 1);
  assert(host.fs.state()->openHandles == 0);
}

void testAbortWaitsForOwnerScopedPhysicalCleanup() {
  FakeHost host;
  host.fs.state()->seed("/large.wav", "01234567");
  LinkFileTransfer transfer;
  transfer.bind(host);
  assert(transfer.send(9, "/large.wav"));

  std::atomic<bool> leaseReady{false};
  std::atomic<bool> releaseLease{false};
  std::thread other([&]() {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        StorageOwner::fontRead, StorageAccess::read, 50);
    assert(lease);
    leaseReady.store(true);
    while (!releaseLease.load()) std::this_thread::yield();
  });
  while (!leaseReady.load()) std::this_thread::yield();

  transfer.abort();
  assert(transfer.cleanupPending());
  assert(host.resources);
  assert(host.fetched.empty());
  assert(host.cancelTransmit == 1);

  releaseLease.store(true);
  other.join();
  assert(transfer.pollCleanup());
  assert(transfer.quiesced());
  assert(!host.resources);
  assert(host.fetched.empty());
  assert(host.fs.state()->openHandles == 0);
}

void testAdmissionErrorsDoNotAcquireResources() {
  FakeHost host;
  LinkFileTransfer transfer;
  transfer.bind(host);

  host.permitted = false;
  assert(!transfer.send(10, "/missing"));
  assert(host.deadlineCancelled == 10);
  assert(!host.resources);

  host.permitted = true;
  assert(!transfer.send(11, "/missing"));
  assert(host.error == 11);
  assert(host.errorMessage == "file is missing");
  assert(!host.resources);
}

}  // namespace

int main() {
  testStreamsOneOwnerAndCompletesAfterCleanup();
  testQueueFailureNeverPublishesResult();
  testEmptyFileUsesTerminalResponseAndCompletesAfterCleanup();
  testBusyPhysicalIoRetriesWithoutDuplicatingBytes();
  testFinalFrameDisconnectBeforeCleanupNeverPublishesResult();
  testDataQueueFailureDisconnectsAndReleasesEverything();
  testAbortWaitsForOwnerScopedPhysicalCleanup();
  testAdmissionErrorsDoNotAcquireResources();
  return 0;
}
