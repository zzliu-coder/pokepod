#include <assert.h>
#include <stdint.h>

#include "AudioCaptureRouter.h"
#include "BleVoiceProtocol.h"
#include "VoiceSessionController.h"

using namespace pokepod;

int main() {
  assert(kBleVoiceFrameBytes == 175);
  assert(kBleVoiceHeaderBytes == 15);
  assert(kBleVoicePayloadBytes == 160);
  assert(static_cast<uint8_t>(BleVoiceCommandType::ready) == 1);
  assert(static_cast<uint8_t>(BleVoiceCommandType::reject) == 2);
  assert(static_cast<uint8_t>(BleVoiceCommandType::stopAck) == 3);
  assert(static_cast<uint8_t>(BleVoiceCommandType::ping) == 4);
  assert(static_cast<uint8_t>(BleVoiceEventType::sessionStart) == 1);
  assert(static_cast<uint8_t>(BleVoiceEventType::sessionEnd) == 2);
  assert(static_cast<uint8_t>(BleVoiceEventType::status) == 3);
  assert(static_cast<uint8_t>(BleVoiceEventType::error) == 4);
  assert(bleVoiceCommandTargetsSession(7, 7));
  assert(!bleVoiceCommandTargetsSession(0, 7));
  assert(!bleVoiceCommandTargetsSession(6, 7));
  assert(!bleVoiceMtuReady(184));
  assert(bleVoiceMtuReady(185));

  int16_t pcm[kBleVoiceSamplesPerFrame];
  for (size_t index = 0; index < kBleVoiceSamplesPerFrame; ++index) {
    pcm[index] = static_cast<int16_t>((static_cast<int>(index) - 160) * 96);
  }
  BleVoiceAudioFrame frame;
  assert(encodeBleVoiceAudio(0x78563412, 0xfffffffe, pcm, frame, 3));
  assert(frame.bytes[0] == 1);
  assert(frame.bytes[1] == 3);
  assert(readVoiceU32(frame.bytes + 2) == 0x78563412);
  assert(readVoiceU32(frame.bytes + 6) == 0xfffffffe);
  assert(readVoiceU16(frame.bytes + 10) == 320);
  int16_t decoded[kBleVoiceSamplesPerFrame] = {};
  uint32_t session = 0;
  uint32_t sequence = 0;
  assert(decodeBleVoiceAudio(frame.bytes, sizeof(frame.bytes), session,
                             sequence, decoded, 320));
  assert(session == 0x78563412);
  assert(sequence == 0xfffffffe);
  assert(decoded[0] == pcm[0]);
  int maximumError = 0;
  for (size_t index = 0; index < 320; ++index) {
    int error = decoded[index] - pcm[index];
    if (error < 0) error = -error;
    if (error > maximumError) maximumError = error;
  }
  assert(maximumError < 1200);
  frame.bytes[0] = 2;
  assert(!decodeBleVoiceAudio(frame.bytes, sizeof(frame.bytes), session,
                              sequence, decoded, 320));
  frame.bytes[0] = 1;
  assert(!decodeBleVoiceAudio(frame.bytes, sizeof(frame.bytes) - 1, session,
                              sequence, decoded, 320));

  int16_t silence[kBleVoiceSamplesPerFrame] = {};
  assert(encodeBleVoiceAudio(3, 4, silence, frame));
  assert(readVoiceU16(frame.bytes + 12) == 0 && frame.bytes[14] == 0);
  for (size_t index = kBleVoiceHeaderBytes; index < sizeof(frame.bytes);
       ++index) assert(frame.bytes[index] == 0);
  assert(decodeBleVoiceAudio(frame.bytes, sizeof(frame.bytes), session,
                             sequence, decoded, 320));
  for (int16_t value : decoded) assert(value == 0);

  BleVoiceControl control;
  control.type = static_cast<uint8_t>(BleVoiceCommandType::ready);
  control.sessionId = 42;
  control.code = 185;
  uint8_t controlBytes[8];
  assert(encodeBleVoiceControl(control, controlBytes, sizeof(controlBytes)) == 8);
  BleVoiceControl restored;
  assert(decodeBleVoiceControl(controlBytes, sizeof(controlBytes), restored));
  assert(restored.sessionId == 42 && restored.code == 185);
  controlBytes[0] = 9;
  assert(!decodeBleVoiceControl(controlBytes, sizeof(controlBytes), restored));

  BleVoiceFrameQueue<2> queue;
  assert(queue.push(frame));
  assert(queue.push(frame));
  assert(!queue.push(frame));
  assert(queue.overflowCount() == 1);
  BleVoiceAudioFrame popped;
  assert(queue.pop(popped));
  assert(queue.pop(popped));
  assert(!queue.pop(popped));

  AudioCaptureRouter router;
  assert(router.acquire(AudioCaptureOwner::localCapsule));
  assert(!router.acquire(AudioCaptureOwner::wirelessVoice));
  router.release(AudioCaptureOwner::localCapsule);
  VoiceSessionController controller;
  assert(!controller.begin(1, 0, false, 185, router));
  assert(controller.error() == VoiceSessionError::notConnected);
  assert(!controller.begin(1, 0, true, 184, router));
  assert(controller.error() == VoiceSessionError::mtuTooSmall);
  assert(controller.begin(6, 10, true, 185, router));
  assert(controller.state() == VoiceSessionState::waitingForReady);
  assert(!controller.markReady(7, 20));
  assert(controller.state() == VoiceSessionState::waitingForReady);
  assert(!controller.poll(410));
  assert(controller.error() == VoiceSessionError::readyTimeout);
  controller.complete();
  assert(controller.begin(7, 10, true, 185, router));
  assert(router.wirelessStreaming());
  assert(controller.state() == VoiceSessionState::waitingForReady);
  const uint32_t waitingSession = controller.sessionId();
  const size_t waitingQueue = controller.queuedFrames();
  assert(!controller.begin(99, 11, true, 185, router));
  assert(controller.state() == VoiceSessionState::waitingForReady);
  assert(controller.sessionId() == waitingSession);
  assert(controller.queuedFrames() == waitingQueue);
  assert(router.wirelessStreaming());
  assert(controller.markReady(7, 20));
  assert(controller.state() == VoiceSessionState::streaming);
  const size_t streamingQueue = controller.queuedFrames();
  assert(!bleVoiceCommandTargetsSession(0, controller.sessionId()));
  assert(!bleVoiceCommandTargetsSession(6, controller.sessionId()));
  assert(controller.state() == VoiceSessionState::streaming);
  assert(controller.sessionId() == 7);
  assert(controller.queuedFrames() == streamingQueue);
  assert(router.wirelessStreaming());
  assert(!controller.begin(99, 21, true, 185, router));
  assert(controller.state() == VoiceSessionState::streaming);
  assert(controller.sessionId() == waitingSession);
  assert(router.wirelessStreaming());
  uint8_t stereo[192] = {};
  for (int chunk = 0; chunk < 25; ++chunk) {
    assert(controller.appendStereo48(stereo, sizeof(stereo), 21 + chunk));
  }
  assert(controller.queuedFrames() > 0);
  controller.end();
  assert(controller.state() == VoiceSessionState::ending);
  assert(router.available());
  while (controller.takeFrame(popped)) {}
  assert(controller.queuedFrames() == 0);
  controller.complete();
  assert(controller.state() == VoiceSessionState::idle);

  VoiceSessionController monoController;
  assert(monoController.begin(67, 0, true, 185, router));
  assert(monoController.markReady(67, 1));
  int16_t monoSamples[kBleVoiceSamplesPerFrame] = {};
  for (size_t index = 0; index < kBleVoiceSamplesPerFrame; ++index) {
    monoSamples[index] = static_cast<int16_t>(index * 3 - 400);
  }
  assert(monoController.appendMono16(
      monoSamples, kBleVoiceSamplesPerFrame, 2));
  assert(monoController.queuedFrames() == 1);
  BleVoiceAudioFrame monoFrame;
  assert(monoController.peekFrame(monoFrame));
  assert(readVoiceU32(monoFrame.bytes + 2) == 67);
  assert(readVoiceU32(monoFrame.bytes + 6) == 0);
  monoController.complete();

  VoiceSessionController streamTimeout;
  assert(streamTimeout.begin(68, 0, true, 185, router));
  assert(streamTimeout.markReady(68, 1));
  assert(streamTimeout.state() == VoiceSessionState::streaming);
  assert(streamTimeout.poll(400));
  assert(!streamTimeout.poll(401));
  assert(streamTimeout.error() == VoiceSessionError::streamTimeout);
  assert(router.available());
  streamTimeout.complete();

  // A quick release before ready keeps buffered speech private until the
  // matching session is authorized, then drains it before session-end.
  VoiceSessionController quickRelease;
  assert(quickRelease.begin(70, 0, true, 185, router));
  for (int chunk = 0; chunk < 25; ++chunk) {
    assert(quickRelease.appendStereo48(stereo, sizeof(stereo), 1 + chunk));
  }
  quickRelease.end();
  assert(quickRelease.state() == VoiceSessionState::waitingForReady);
  assert(!quickRelease.acceptsAudio());
  const size_t quickReleaseQueue = quickRelease.queuedFrames();
  assert(!quickRelease.begin(72, 29, true, 185, router));
  assert(quickRelease.state() == VoiceSessionState::waitingForReady);
  assert(quickRelease.sessionId() == 70);
  assert(quickRelease.queuedFrames() == quickReleaseQueue);
  assert(!quickRelease.appendStereo48(stereo, sizeof(stereo), 30));
  assert(quickRelease.queuedFrames() == quickReleaseQueue);
  assert(router.available());
  assert(!quickRelease.takeFrame(popped));
  assert(!quickRelease.markReady(71, 30));
  assert(!quickRelease.takeFrame(popped));
  assert(quickRelease.markReady(70, 31));
  assert(quickRelease.state() == VoiceSessionState::ending);
  size_t drained = 0;
  while (quickRelease.takeFrame(popped)) ++drained;
  assert(drained > 0);
  assert(quickRelease.markSessionEndSent(32));
  assert(quickRelease.state() == VoiceSessionState::awaitingStopAck);
  assert(!quickRelease.acceptsStopAck(0));
  assert(!quickRelease.acceptsStopAck(69));
  assert(quickRelease.acceptsStopAck(70));
  assert(!quickRelease.markSessionEndSent(33));
  assert(!quickRelease.begin(72, 34, true, 185, router));
  assert(quickRelease.sessionId() == 70);
  assert(quickRelease.state() == VoiceSessionState::awaitingStopAck);
  assert(!quickRelease.acceptsStopAck(0));
  assert(!quickRelease.acceptsStopAck(69));
  assert(quickRelease.state() == VoiceSessionState::awaitingStopAck);
  assert(quickRelease.acceptsStopAck(70));
  quickRelease.complete();
  assert(quickRelease.state() == VoiceSessionState::idle);

  VoiceSessionController endingSession;
  assert(endingSession.begin(73, 0, true, 185, router));
  assert(endingSession.markReady(73, 1));
  for (int chunk = 0; chunk < 25; ++chunk) {
    assert(endingSession.appendStereo48(stereo, sizeof(stereo), 2 + chunk));
  }
  endingSession.end();
  assert(endingSession.state() == VoiceSessionState::ending);
  const size_t endingQueue = endingSession.queuedFrames();
  assert(!endingSession.begin(74, 30, true, 185, router));
  assert(endingSession.state() == VoiceSessionState::ending);
  assert(endingSession.sessionId() == 73);
  assert(endingSession.queuedFrames() == endingQueue);
  assert(router.available());
  endingSession.complete();

  VoiceSessionController missingAck;
  assert(missingAck.begin(75, 0, true, 185, router));
  assert(missingAck.markReady(75, 1));
  missingAck.end();
  assert(missingAck.state() == VoiceSessionState::ending);
  assert(missingAck.markSessionEndSent(10));
  assert(missingAck.state() == VoiceSessionState::awaitingStopAck);
  assert(missingAck.poll(1009));
  assert(!missingAck.poll(1010));
  assert(missingAck.state() == VoiceSessionState::idle);
  assert(missingAck.error() == VoiceSessionError::stopAckTimeout);
  assert(router.available());
  assert(missingAck.begin(76, 1011, true, 185, router));
  missingAck.complete();

  VoiceSessionController quickTimeout;
  assert(quickTimeout.begin(71, 0, true, 185, router));
  for (int chunk = 0; chunk < 25; ++chunk) {
    assert(quickTimeout.appendStereo48(stereo, sizeof(stereo), 1 + chunk));
  }
  quickTimeout.end();
  assert(!quickTimeout.takeFrame(popped));
  assert(!quickTimeout.poll(400));
  assert(quickTimeout.error() == VoiceSessionError::readyTimeout);
  assert(quickTimeout.queuedFrames() == 0);
  assert(!quickTimeout.takeFrame(popped));
  quickTimeout.complete();

  VoiceSessionController overflow;
  assert(overflow.begin(8, 0, true, 185, router));
  assert(overflow.state() == VoiceSessionState::waitingForReady);
  // 24 independent 20 ms frames cover the full 400 ms ready timeout.
  size_t bufferedChunks = 0;
  while (overflow.queuedFrames() < VoiceSessionController::kQueueFrames) {
    assert(overflow.appendStereo48(stereo, sizeof(stereo),
                                  1 + static_cast<uint32_t>(bufferedChunks)));
    assert(++bufferedChunks < 600);
  }
  assert(overflow.queuedFrames() == VoiceSessionController::kQueueFrames);
  assert(overflow.poll(399));
  assert(overflow.markReady(8, 399));
  assert(overflow.state() == VoiceSessionState::streaming);
  // The queue accepts exactly 24 frames; frame 25 is the overflow boundary.
  bool overflowed = false;
  for (size_t chunk = 0; chunk < 30; ++chunk) {
    if (!overflow.appendStereo48(stereo, sizeof(stereo), 400 + chunk)) {
      overflowed = true;
      break;
    }
  }
  assert(overflowed);
  assert(overflow.error() == VoiceSessionError::queueOverflow);
  assert(router.available());
  return 0;
}
