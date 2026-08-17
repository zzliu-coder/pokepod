#include <cassert>

#include "AudioCaptureRouter.h"
#include "BleCallbackOverflowPolicy.h"
#include "BleServiceEnablePolicy.h"
#include "VoiceSessionController.h"

using namespace pokepod;

int main() {
  AudioCaptureRouter router;
  VoiceSessionController session;
  BleServiceEnablePolicy enable;
  enable.begin(true);

  // Model both touch and BOOT voice entrypoints at their shared controller
  // boundary. Disable requests capture stop while App retains sole router
  // ownership through the capture-runtime stop and drain boundary.
  assert(router.acquire(AudioCaptureOwner::wirelessVoice));
  assert(session.begin(41, 100, true, 185, router));
  assert(session.markReady(41, 101));
  assert(router.owner() == AudioCaptureOwner::wirelessVoice);
  auto actions = enable.requestDisable(session.active(), true, 102);
  assert(actions.requestSessionStop);
  assert(!actions.disconnect);
  assert(!enable.acceptsNewWork());
  session.end();
  assert(router.wirelessStreaming());
  router.release(AudioCaptureOwner::wirelessVoice);
  assert(router.available());
  assert(session.state() == VoiceSessionState::ending);
  assert(session.markSessionEndSent(103));
  assert(session.state() == VoiceSessionState::awaitingStopAck);
  assert(!session.poll(103 + VoiceSessionController::kStopAckTimeoutMs));
  assert(session.state() == VoiceSessionState::idle);
  actions = enable.poll(session.active(), true, 1200);
  assert(actions.disconnect);
  actions = enable.poll(session.active(), false, 1201);
  assert(actions.clearRuntime);
  assert(enable.phase() == BleServiceEnablePhase::disabled);
  assert(router.available());

  // A terminal queue overflow cannot release capture before App drains it.
  enable.requestEnable(false);
  assert(router.acquire(AudioCaptureOwner::wirelessVoice));
  assert(session.begin(42, 2000, true, 185, router));
  assert(session.markReady(42, 2001));
  int16_t samples[kBleVoiceSamplesPerFrame] = {};
  bool overflowed = false;
  for (size_t frame = 0; frame <= VoiceSessionController::kQueueFrames;
       ++frame) {
    if (!session.appendMono16(samples, kBleVoiceSamplesPerFrame, 2002)) {
      overflowed = true;
      break;
    }
  }
  assert(overflowed);
  assert(session.error() == VoiceSessionError::queueOverflow);
  assert(!session.active());
  assert(router.wirelessStreaming());
  router.release(AudioCaptureOwner::wirelessVoice);
  assert(router.available());
  actions = enable.requestDisable(session.active(), true, 2003);
  assert(actions.disconnect);
  actions = enable.poll(session.active(), false, 2004);
  assert(actions.clearRuntime);
  assert(router.available());

  // active session -> disable -> callback overflow -> late exact physical
  // disconnect -> re-enable. Advertising is allowed only after the overflow
  // epoch has physically closed and the old connection policy can be cleared.
  enable.begin(true);
  assert(router.acquire(AudioCaptureOwner::wirelessVoice));
  assert(session.begin(43, 3000, true, 185, router));
  assert(session.markReady(43, 3001));
  actions = enable.requestDisable(session.active(), true, 3002);
  assert(actions.requestSessionStop);
  BleCallbackOverflowPolicy overflowPolicy;
  const BleVoiceConnectionEpoch overflowEpoch{9, 4};
  assert(overflowPolicy.begin(overflowEpoch, 3003));
  assert(overflowPolicy.poll(3003).disconnect);
  session.abort(VoiceSessionError::disconnected);
  assert(session.state() == VoiceSessionState::failed);
  assert(session.error() == VoiceSessionError::disconnected);
  assert(router.owner() == AudioCaptureOwner::wirelessVoice);
  assert(!session.markSessionEndSent(3003));
  // The callback cleanup marks the BLE session failed. App remains the sole
  // owner of capture/router teardown and releases it at its existing drain
  // boundary, so overflow cannot manufacture a successful completion.
  router.release(AudioCaptureOwner::wirelessVoice);
  assert(router.available());
  actions = enable.poll(session.active(),
                        overflowPolicy.physicalConnectionPending(), 3003);
  assert(actions.disconnect);
  // The first controller terminate may produce no callback. Keep the frozen
  // epoch physically pending and retry it on the bounded cadence.
  actions = enable.poll(session.active(),
                        overflowPolicy.physicalConnectionPending(), 3252);
  assert(!actions.disconnect);
  assert(!overflowPolicy.poll(3252).disconnect);
  actions = enable.poll(session.active(),
                        overflowPolicy.physicalConnectionPending(), 3253);
  assert(actions.disconnect);
  assert(overflowPolicy.poll(3253).disconnect);
  enable.requestEnable(true);
  assert(enable.transitionPending());
  assert(!enable.acceptsNewWork());
  assert(!overflowPolicy.confirm(BleVoiceConnectionEpoch{9, 3}));
  actions = enable.poll(session.active(),
                        overflowPolicy.physicalConnectionPending(), 3503);
  assert(actions.disconnect);
  assert(overflowPolicy.confirm(BleVoiceConnectionEpoch{9, 4}));
  assert(overflowPolicy.finish().matches(overflowEpoch));
  actions = enable.poll(session.active(),
                        overflowPolicy.physicalConnectionPending(), 3004);
  assert(actions.clearRuntime);
  assert(actions.startAdvertising);
  assert(enable.acceptsNewWork());

  return 0;
}
