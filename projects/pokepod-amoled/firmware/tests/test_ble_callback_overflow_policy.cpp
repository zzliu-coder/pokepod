#include <cassert>
#include <cstdint>

#include "BleCallbackOverflowPolicy.h"
#include "BleServiceEnablePolicy.h"

using namespace pokepod;

namespace {

void assertNotQuiescedByEachBlocker() {
  BleSleepQuiescenceFacts facts;
  assert(bleVoiceQuiescedForSleep(facts));

  for (uint32_t blocker = 0; blocker < 7; ++blocker) {
    BleSleepQuiescenceFacts blocked;
    switch (blocker) {
      case 0: blocked.sessionActive = true; break;
      case 1: blocked.pairingActive = true; break;
      case 2: blocked.enableTransitionPending = true; break;
      case 3: blocked.overflowCleanupActive = true; break;
      case 4: blocked.physicalConnectionPending = true; break;
      case 5: blocked.notifyPending = true; break;
      case 6: blocked.callbackMailboxEmpty = false; break;
    }
    assert(!bleVoiceQuiescedForSleep(blocked));
  }
  facts.notifyMailboxEmpty = false;
  assert(!bleVoiceQuiescedForSleep(facts));
}

}  // namespace

int main() {
  const BleVoiceConnectionEpoch first{7, 70};
  BleCallbackOverflowPolicy policy;

  // Enabled service overflow: the cleanup owns its first disconnect and every
  // retry, independent of the user-intent policy remaining enabled.
  BleServiceEnablePolicy enabled;
  enabled.begin(true);
  assert(enabled.phase() == BleServiceEnablePhase::enabled);
  assert(policy.begin(first, 1000));
  auto cleanup = policy.poll(1000);
  assert(cleanup.disconnect);
  assert(policy.attempts() == 1);
  assert(!enabled.poll(false, true, 1000).disconnect);
  assert(!policy.poll(1249).disconnect);
  cleanup = policy.poll(1250);
  assert(cleanup.disconnect);
  assert(policy.attempts() == 2);

  // A reused handle, old generation, other ID and duplicate observation do
  // not settle the frozen epoch.
  assert(!policy.confirm(BleVoiceConnectionEpoch{7, 69}));
  assert(!policy.confirm(BleVoiceConnectionEpoch{8, 70}));
  assert(policy.phase() == BleCallbackOverflowPhase::disconnecting);
  assert(policy.confirm(first));
  assert(!policy.confirm(first));
  assert(policy.finish().matches(first));
  assert(policy.phase() == BleCallbackOverflowPhase::idle);
  assert(!policy.physicalConnectionPending());

  // A rapid disable/enable changes only final user intent. Cleanup continues
  // to retry its frozen ID while ordinary enable teardown remains separate.
  const BleVoiceConnectionEpoch second{9, 4};
  assert(policy.begin(second, 2000));
  assert(policy.poll(2000).disconnect);
  auto enableActions = enabled.requestDisable(false, true, 2001);
  assert(enableActions.disconnect);
  enableActions = enabled.requestEnable(true);
  assert(!enableActions.startAdvertising);
  assert(enabled.userEnabled());
  assert(enabled.transitionPending());
  assert(policy.poll(2249).disconnect == false);
  assert(policy.poll(2250).disconnect);
  assert(policy.confirm(second));
  assert(policy.finish().matches(second));
  enableActions = enabled.poll(false, false, 2251);
  assert(enableActions.clearRuntime);
  assert(enableActions.startAdvertising);
  assert(enabled.acceptsNewWork());

  // A second overflow creates a fresh epoch. It does not inherit the previous
  // attempt count or accept its old disconnect fact.
  const BleVoiceConnectionEpoch third{9, 5};
  assert(policy.begin(third, 3000));
  assert(policy.attempts() == 0);
  assert(!policy.confirm(second));
  assert(policy.poll(3000).disconnect);
  assert(policy.attempts() == 1);
  assert(policy.confirm(third));
  assert(policy.finish().matches(third));

  // Absolute deadline wins over a retry due at the same timestamp and leaves
  // admission in an explicit, observable hard-failed terminal state.
  const BleVoiceConnectionEpoch deadlineEpoch{11, 8};
  assert(policy.begin(deadlineEpoch, 4000));
  assert(policy.poll(4000).disconnect);
  cleanup = policy.poll(
      4000 + BleCallbackOverflowPolicy::kCleanupDeadlineMs - 1);
  assert(cleanup.disconnect);
  cleanup = policy.poll(
      4000 + BleCallbackOverflowPolicy::kCleanupDeadlineMs);
  assert(!cleanup.disconnect);
  assert(cleanup.enteredHardFailed);
  assert(policy.hardFailed());
  assert(policy.requiresProcessRecovery());
  assert(policy.physicalConnectionPending());
  assert(!policy.confirm(BleVoiceConnectionEpoch{11, 7}));
  assert(!policy.finish().valid());
  assert(policy.hardFailureCount() == 1);
  // A delayed exact physical disconnect remains a valid recovery fact even
  // after the absolute deadline; stale epochs still cannot rearm the queues.
  assert(policy.confirm(deadlineEpoch));
  assert(policy.finish().matches(deadlineEpoch));
  assert(!policy.active());

  // uint32_t wrap does not move either the retry or deadline boundary.
  BleCallbackOverflowPolicy wrapped;
  const BleVoiceConnectionEpoch wrapEpoch{15, 99};
  constexpr uint32_t kStarted = UINT32_MAX - 100U;
  assert(wrapped.begin(wrapEpoch, kStarted));
  assert(wrapped.poll(kStarted).disconnect);
  assert(!wrapped.poll(kStarted + 249U).disconnect);
  assert(wrapped.poll(kStarted + 250U).disconnect);
  cleanup = wrapped.poll(
      kStarted + BleCallbackOverflowPolicy::kCleanupDeadlineMs);
  assert(cleanup.enteredHardFailed);
  assert(wrapped.hardFailed());

  // An overflow without a trustworthy physical epoch fails closed immediately
  // and cannot fabricate a disconnect confirmation.
  BleCallbackOverflowPolicy invalid;
  assert(invalid.begin({}, 10));
  assert(invalid.hardFailed());
  assert(!invalid.requiresProcessRecovery());
  assert(!invalid.physicalConnectionPending());
  assert(invalid.hardFailureCount() == 1);
  assert(invalid.recoverInvalidEpoch());
  assert(!invalid.active());
  assert(invalid.begin(BleVoiceConnectionEpoch{21, 1}, 20));

  // App deep-sleep admission reads these facts after logical connected has
  // already gone false. Both live cleanup and hard-failed overflow remain
  // blockers. Exact disconnect, mailbox reset and idle pause leave the clean
  // all-false fact set that may proceed.
  BleSleepQuiescenceFacts disconnectingSleep;
  disconnectingSleep.overflowCleanupActive = true;
  disconnectingSleep.physicalConnectionPending = true;
  assert(!bleVoiceQuiescedForSleep(disconnectingSleep));
  BleSleepQuiescenceFacts hardFailedSleep;
  hardFailedSleep.overflowCleanupActive = true;
  assert(!bleVoiceQuiescedForSleep(hardFailedSleep));
  BleSleepQuiescenceFacts exactDisconnectResetAndPaused;
  assert(bleVoiceQuiescedForSleep(exactDisconnectResetAndPaused));

  assertNotQuiescedByEachBlocker();
  return 0;
}
