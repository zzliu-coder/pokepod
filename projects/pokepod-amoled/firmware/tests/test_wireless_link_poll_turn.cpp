#include <algorithm>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../PokePodAmoled/LinkBoundedTextRead.h"
#include "../PokePodAmoled/WirelessLinkPollTurn.h"

using namespace pokepod;

namespace {

class MetadataContinuationLink {
 public:
  explicit MetadataContinuationLink(size_t firstBytes,
                                    size_t secondBytes = 0)
      : secondBytes_(secondBytes) {
    assert(reader_.begin(firstBytes));
  }

  void poll(uint32_t) {
    ++fullPolls_;
    pollDeferredCleanup();
  }

  void pollDeferredCleanup() {
    ++cleanupPolls_;
    if (!reader_.active()) return;
    const size_t bytes = reader_.nextReadBytes();
    assert(bytes <= LinkBoundedTextRead::kBytesPerPoll);
    ++readCalls_;
    bytesReadThisTurn_ += bytes;
    assert(reader_.acceptRead(bytes));
    if (reader_.ready() && secondBytes_ > 0) {
      // Production batch continuations stage the second path when the first
      // becomes ready. The next outer turn must own its first read.
      reader_.reset();
      assert(reader_.begin(secondBytes_));
      secondBytes_ = 0;
    }
  }

  void beginOuterTurn() { bytesReadThisTurn_ = 0; }
  size_t bytesReadThisTurn() const { return bytesReadThisTurn_; }
  size_t readCalls() const { return readCalls_; }
  size_t fullPolls() const { return fullPolls_; }
  size_t cleanupPolls() const { return cleanupPolls_; }

 private:
  LinkBoundedTextRead reader_;
  size_t secondBytes_ = 0;
  size_t bytesReadThisTurn_ = 0;
  size_t readCalls_ = 0;
  size_t fullPolls_ = 0;
  size_t cleanupPolls_ = 0;
};

}  // namespace

int main() {
  MetadataContinuationLink large(8192);
  for (unsigned turn = 0; turn < 8; ++turn) {
    large.beginOuterTurn();
    {
      WirelessLinkPollTurn<MetadataContinuationLink> outer(large);
      outer.pollAuthenticated(turn);
      assert(outer.fullPollRan());
    }
    assert(large.bytesReadThisTurn() <= 1024);
    assert(large.readCalls() == turn + 1);
  }
  assert(large.fullPolls() == 8);
  assert(large.cleanupPolls() == 8);

  // Finishing capsule.json stages processing.json, but the authenticated
  // outer poll cannot invoke cleanup a second time and read that next file.
  MetadataContinuationLink twoFiles(1024, 1024);
  twoFiles.beginOuterTurn();
  {
    WirelessLinkPollTurn<MetadataContinuationLink> outer(twoFiles);
    outer.pollAuthenticated(1);
  }
  assert(twoFiles.readCalls() == 1);
  assert(twoFiles.bytesReadThisTurn() == 1024);
  twoFiles.beginOuterTurn();
  {
    WirelessLinkPollTurn<MetadataContinuationLink> outer(twoFiles);
    outer.pollAuthenticated(2);
  }
  assert(twoFiles.readCalls() == 2);
  assert(twoFiles.bytesReadThisTurn() == 1024);

  // An unauthenticated/window-closed turn still advances cleanup exactly once.
  MetadataContinuationLink cleanupOnly(1024);
  cleanupOnly.beginOuterTurn();
  { WirelessLinkPollTurn<MetadataContinuationLink> outer(cleanupOnly); }
  assert(cleanupOnly.fullPolls() == 0);
  assert(cleanupOnly.cleanupPolls() == 1);
  assert(cleanupOnly.readCalls() == 1);
  assert(cleanupOnly.bytesReadThisTurn() == 1024);
  return 0;
}
