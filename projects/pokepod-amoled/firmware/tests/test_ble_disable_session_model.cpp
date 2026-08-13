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
  overflowPolicy.begin(overflowEpoch);
  session.abort(VoiceSessionError::disconnected);
  router.release(AudioCaptureOwner::wirelessVoice);
  actions = enable.poll(session.active(),
                        overflowPolicy.physicalConnectionPending(), 3003);
  assert(actions.disconnect);
  enable.requestEnable(true);
  assert(enable.transitionPending());
  assert(!enable.acceptsNewWork());
  assert(!overflowPolicy.confirm(BleVoiceConnectionEpoch{9, 3}));
  assert(overflowPolicy.confirm(BleVoiceConnectionEpoch{9, 4}));
  assert(overflowPolicy.finish().matches(overflowEpoch));
  actions = enable.poll(session.active(),
                        overflowPolicy.physicalConnectionPending(), 3004);
  assert(actions.clearRuntime);
  assert(actions.startAdvertising);
  assert(enable.acceptsNewWork());

  return 0;
}
