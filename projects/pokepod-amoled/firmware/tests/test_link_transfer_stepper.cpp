#include <assert.h>
#include <stdint.h>

#include "../PokePodAmoled/LinkTransferStepper.h"
#include "../PokePodAmoled/LinkTransferGate.h"
#include "../PokePodAmoled/MaintenanceCompletionTracker.h"

using namespace pokepod;

namespace {

LinkWriteAttempt wrote(size_t count) {
  return {LinkWriteDisposition::progress, count};
}

void testOneBoundedAttemptPerPoll() {
  LinkTransferStepper stepper;
  assert(stepper.beginFrame(16404));
  size_t polls = 0;
  while (stepper.active()) {
    const size_t wanted = stepper.nextWriteBytes(true, 512);
    assert(wanted > 0 && wanted <= 512);
    const LinkTransferStepResult result = stepper.accept(true, wrote(wanted));
    assert(result == LinkTransferStepResult::progress ||
           result == LinkTransferStepResult::frameComplete);
    ++polls;
  }
  assert(polls == 33);
}

void testBackpressureDoesNotAdvance() {
  LinkTransferStepper stepper;
  assert(stepper.beginFrame(1024));
  assert(stepper.accept(true, {LinkWriteDisposition::wouldBlock, 0}) ==
         LinkTransferStepResult::wouldBlock);
  assert(stepper.offset() == 0);
  assert(stepper.nextWriteBytes(true, 512) == 512);
}

void testAbsoluteDeadlineCancelsInsideFrame() {
  LinkTransferStepper stepper;
  assert(stepper.beginFrame(16404));
  struct Cancellation final : LinkTransferCancellationSink {
    void cancelForTransferDeadline() override { ++calls; }
    unsigned calls = 0;
  } cancellation;
  AbsoluteLinkDeadlineGate gate;
  gate.attachCancellationSink(&cancellation);
  gate.arm(0, 300000);
  uint32_t nowMs = 299000;
  while (nowMs < 300000) {
    const bool permitted = gate.permits(nowMs);
    const size_t wanted = stepper.nextWriteBytes(permitted, 128);
    assert(wanted <= 128);
    assert(stepper.accept(permitted, wrote(wanted)) !=
           LinkTransferStepResult::cancelled);
    nowMs += 100;
  }
  assert(!gate.permits(300000));
  assert(cancellation.calls == 1);
  assert(stepper.accept(false, {LinkWriteDisposition::wouldBlock, 0}) ==
         LinkTransferStepResult::cancelled);
  assert(!stepper.active());
}

void testReadOnlyMaintenanceCompletesOnlyAfterFinalFrame() {
  static constexpr const char *kTransaction =
      "22222222-2222-4222-8222-222222222222";
  MaintenanceCompletionTracker tracker;
  tracker.beginAccepted();
  tracker.endResultPersisted(kTransaction);

  LinkTransferStepper stepper;
  constexpr size_t kAudioBytes = 3U * 1024U * 1024U;
  constexpr size_t kFramePayload = 16384;
  size_t queued = 0;
  size_t writeAttempts = 0;
  size_t polls = 0;
  bool responseSent = false;
  while (!responseSent || queued < kAudioBytes || stepper.active()) {
    if (!stepper.active()) {
      const size_t payload = !responseSent ? 80 :
          ((kAudioBytes - queued) < kFramePayload
               ? kAudioBytes - queued : kFramePayload);
      assert(stepper.beginFrame(20 + payload));
      if (responseSent) queued += payload;
      else responseSent = true;
    }
    ++polls;
    const size_t wanted = stepper.nextWriteBytes(true, 512);
    assert(wanted <= 512);
    ++writeAttempts;
    const LinkTransferStepResult result = stepper.accept(true, wrote(wanted));
    if (queued < kAudioBytes || stepper.active()) {
      assert(tracker.completionRevision() == 0);
    }
    if (result == LinkTransferStepResult::frameComplete &&
        queued == kAudioBytes) {
      assert(tracker.resultFetched(kTransaction, true));
    }
  }
  assert(!stepper.active());
  assert(writeAttempts == polls);
  assert(tracker.completionRevision() == 1);
  tracker.disconnect();
  assert(tracker.completionRevision() == 1);
}

void testUsbHasNoWirelessDeadline() {
  LinkTransferStepper stepper;
  assert(stepper.beginFrame(4096));
  uint32_t nowMs = 299000;
  while (stepper.active()) {
    // A null Wi-Fi gate is represented by permitted=true for the USB owner.
    const size_t wanted = stepper.nextWriteBytes(true, 512);
    const LinkTransferStepResult result = stepper.accept(true, wrote(wanted));
    assert(result != LinkTransferStepResult::cancelled);
    nowMs += 1000;
  }
  assert(nowMs > 300000);
}

void testDisconnectAndSecondSession() {
  LinkTransferStepper stepper;
  assert(stepper.beginFrame(4096));
  assert(stepper.accept(true,
                        {LinkWriteDisposition::disconnected, 0}) ==
         LinkTransferStepResult::disconnected);
  assert(!stepper.active());
  assert(stepper.beginFrame(20));
  assert(stepper.offset() == 0);
  assert(stepper.accept(true, wrote(20)) ==
         LinkTransferStepResult::frameComplete);
}

}  // namespace

int main() {
  testOneBoundedAttemptPerPoll();
  testBackpressureDoesNotAdvance();
  testAbsoluteDeadlineCancelsInsideFrame();
  testUsbHasNoWirelessDeadline();
  testDisconnectAndSecondSession();
  testReadOnlyMaintenanceCompletesOnlyAfterFinalFrame();
  return 0;
}
