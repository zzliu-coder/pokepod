#include <assert.h>
#include <stdint.h>

#include "AudioCaptureRouter.h"
#include "BleNotifyReliability.h"
#include "BleVoiceProtocol.h"
#include "BleVoiceQuality.h"
#include "VoiceSessionController.h"

using namespace pokepod;

namespace {

void queueVoiceFrame(VoiceSessionController &controller, uint32_t nowMs) {
  uint8_t stereo[192] = {};
  for (int chunk = 0; chunk < 25; ++chunk) {
    assert(controller.appendStereo48(stereo, sizeof(stereo), nowMs + chunk));
  }
}

}  // namespace

int main() {
  // Queue head stays stable until the host accepts the notification.
  BleVoiceFrameQueue<2> queue;
  int16_t silence[kBleVoiceSamplesPerFrame] = {};
  BleVoiceAudioFrame first;
  BleVoiceAudioFrame second;
  assert(encodeBleVoiceAudio(9, 0xfffffffeU, silence, first));
  assert(encodeBleVoiceAudio(9, 0xffffffffU, silence, second));
  assert(queue.push(first));
  assert(queue.push(second));
  BleVoiceAudioFrame observed;
  assert(queue.peek(observed));
  assert(readVoiceU32(observed.bytes + 6) == 0xfffffffeU);
  assert(queue.peek(observed));
  assert(readVoiceU32(observed.bytes + 6) == 0xfffffffeU);
  assert(queue.commit());
  assert(queue.peek(observed));
  assert(readVoiceU32(observed.bytes + 6) == 0xffffffffU);

  BleNotifyInFlight accepted;
  assert(accepted.start(0xfffffffeU, 100));
  assert(accepted.canAttempt(100));
  assert(accepted.beginAttempt(100));
  assert(!accepted.resolve(false, 101));
  assert(accepted.sequence() == 0xfffffffeU);
  assert(!accepted.canAttempt(108));
  assert(accepted.canAttempt(109));
  assert(accepted.beginAttempt(109));
  assert(accepted.resolve(true, 110));
  assert(accepted.accepted());
  assert(accepted.attempts() == 2);
  assert(!accepted.active());

  // Three host rejections are bounded and leave an explicit failed result.
  BleNotifyInFlight rejected;
  assert(rejected.start(7, 0));
  for (uint32_t attempt = 0; attempt < 3; ++attempt) {
    const uint32_t at = attempt * 9;
    assert(rejected.canAttempt(at));
    assert(rejected.beginAttempt(at));
    assert(!rejected.resolve(false, at + 1));
  }
  assert(rejected.failed());
  assert(rejected.failure() == BleNotifyFailure::rejected);
  assert(rejected.attempts() == BleNotifyInFlight::kMaxAttempts);

  // Missing callbacks time out, retry the same sequence, then fail within the
  // fixed lifetime. The arithmetic remains correct across millis() wrap.
  BleNotifyInFlight timeout;
  const uint32_t nearWrap = 0xfffffff0U;
  assert(timeout.start(0xffffffffU, nearWrap));
  assert(timeout.beginAttempt(nearWrap));
  timeout.poll(nearWrap + 40U);
  assert(timeout.snapshot().state == BleNotifyInFlightState::backoff);
  assert(timeout.sequence() == 0xffffffffU);
  assert(timeout.canAttempt(nearWrap + 48U));
  assert(timeout.beginAttempt(nearWrap + 48U));
  timeout.poll(nearWrap + 88U);
  assert(timeout.canAttempt(nearWrap + 96U));
  assert(timeout.beginAttempt(nearWrap + 96U));
  timeout.poll(nearWrap + 136U);
  assert(timeout.failed());
  assert(timeout.failure() == BleNotifyFailure::timeout);

  BleNotifyInFlight cancelled;
  assert(cancelled.start(3, 10));
  assert(cancelled.beginAttempt(10));
  cancelled.cancel();
  assert(cancelled.failed());
  assert(cancelled.failure() == BleNotifyFailure::cancelled);

  BleVoiceQualityCounters quality;
  quality.recordNotifyAttempt();
  quality.recordNotifyStatus(false);
  quality.recordNotifyRetry();
  quality.recordControlNotifyAttempt();
  quality.recordControlNotifyStatus(true);
  quality.recordSessionError(VoiceSessionError::notifyFailed);
  quality.recordSessionError(VoiceSessionError::disconnected);
  const BleVoiceQualitySnapshot qualityValue = quality.snapshot();
  assert(qualityValue.notifyAttempts == 1);
  assert(qualityValue.notifyFailures == 1);
  assert(qualityValue.notifyRetries == 1);
  assert(qualityValue.notifyAborts == 1);
  assert(qualityValue.disconnectedSessions == 1);
  assert(qualityValue.controlNotifyAttempts == 1);
  assert(qualityValue.controlNotifyAccepted == 1);
  assert(qualityValue.controlNotifyFailures == 0);
  assert(qualityValue.issueCount() == 3);

  // VoiceSessionController exposes peek/commit specifically for the BLE host
  // acceptance boundary. Rejections cannot advance its sequence head.
  AudioCaptureRouter router;
  VoiceSessionController controller;
  assert(router.acquire(AudioCaptureOwner::wirelessVoice));
  assert(controller.begin(91, 0, true, 185, router));
  assert(controller.markReady(91, 1));
  queueVoiceFrame(controller, 2);
  const size_t before = controller.queuedFrames();
  assert(before > 0);
  assert(controller.peekFrame(observed));
  const uint32_t headSequence = readVoiceU32(observed.bytes + 6);
  assert(!controller.commitFrame(headSequence + 1));
  assert(controller.queuedFrames() == before);
  assert(controller.peekFrame(observed));
  assert(readVoiceU32(observed.bytes + 6) == headSequence);
  assert(controller.commitFrame(headSequence));
  assert(controller.queuedFrames() == before - 1);
  if (controller.peekFrame(observed)) {
    assert(readVoiceU32(observed.bytes + 6) == headSequence + 1);
  }

  controller.abort(VoiceSessionError::notifyFailed);
  assert(controller.state() == VoiceSessionState::failed);
  assert(controller.error() == VoiceSessionError::notifyFailed);
  assert(router.wirelessStreaming());
  controller.complete();
  assert(controller.state() == VoiceSessionState::idle);
  router.release(AudioCaptureOwner::wirelessVoice);
  assert(router.available());
  return 0;
}
