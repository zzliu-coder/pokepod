#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../PokePodAmoled/AudioCaptureDispatcher.h"

using namespace pokepod;

namespace {

struct FakeLog {};

class FakeSource {
 public:
  void add(uint32_t sessionId, uint32_t sequence) {
    AudioCaptureFrame frame;
    frame.sessionId = sessionId;
    frame.sequence = sequence;
    frame.capturedAtMs = sequence * 20U;
    frame.samples[0] = static_cast<int16_t>(sequence);
    frames_.push_back(frame);
  }

  bool pop(AudioCaptureFrame &frame) {
    if (next_ >= frames_.size()) return false;
    frame = frames_[next_++];
    poppedSequences.push_back(frame.sequence);
    return true;
  }

  std::vector<uint32_t> poppedSequences;

 private:
  std::vector<AudioCaptureFrame> frames_;
  size_t next_ = 0;
};

class FakeRouter {
 public:
  explicit FakeRouter(AudioCaptureOwner owner) : owner_(owner) {}
  AudioCaptureOwner owner() const { return owner_; }
  void set(AudioCaptureOwner owner) { owner_ = owner; }

 private:
  AudioCaptureOwner owner_ = AudioCaptureOwner::none;
};

class FakeAudio {
 public:
  void observeCapturedMono(const int16_t *, size_t count) {
    assert(count == kAudioCaptureSamplesPerFrame);
    ++observedFrames;
  }
  uint32_t observedFrames = 0;
};

class FakeRecorder {
 public:
  RecorderOperationOwner operationOwner() const { return owner; }
  bool recording() const { return active; }
  bool captureFailureLatched() const {
    return failureStage != RecorderFailureStage::none;
  }
  bool appendMono16(const int16_t *, size_t count, FakeLog &log) {
    assert(count == kAudioCaptureSamplesPerFrame);
    if (acceptedFrames >= capacityFrames) {
      reportCaptureFailure(log, RecorderTerminal::storageFailure,
                           RecorderFailureStage::storageQueueOverflow);
      return false;
    }
    ++acceptedFrames;
    return true;
  }
  void reportCaptureFailure(FakeLog &, RecorderTerminal terminal,
                            RecorderFailureStage stage) {
    if (failureStage == RecorderFailureStage::none) {
      failureTerminal = terminal;
      failureStage = stage;
    }
    active = false;
  }
  void reset(RecorderOperationOwner nextOwner, uint32_t capacity) {
    owner = nextOwner;
    capacityFrames = capacity;
    acceptedFrames = 0;
    failureStage = RecorderFailureStage::none;
    failureTerminal = RecorderTerminal::none;
    active = true;
  }

  RecorderOperationOwner owner = RecorderOperationOwner::localApp;
  RecorderTerminal failureTerminal = RecorderTerminal::none;
  RecorderFailureStage failureStage = RecorderFailureStage::none;
  uint32_t capacityFrames = 16;
  uint32_t acceptedFrames = 0;
  bool active = true;
};

class FakeVoice {
 public:
  bool acceptingAudio() const { return accepting; }
  bool appendMono16(const int16_t *, size_t count, uint32_t) {
    assert(count == kAudioCaptureSamplesPerFrame);
    ++acceptedFrames;
    return !fail;
  }
  bool accepting = false;
  bool fail = false;
  uint32_t acceptedFrames = 0;
};

void assertUnique(const std::vector<uint32_t> &sequences) {
  for (size_t left = 0; left < sequences.size(); ++left) {
    for (size_t right = left + 1; right < sequences.size(); ++right) {
      assert(sequences[left] != sequences[right]);
    }
  }
}

void runLocalAndLinkRouting() {
  AudioCaptureDispatcher dispatcher;
  FakeRouter router(AudioCaptureOwner::localCapsule);
  FakeAudio audio;
  FakeRecorder recorder;
  FakeVoice voice;
  FakeLog log;

  FakeSource local;
  local.add(1, 0);
  local.add(1, 1);
  local.add(1, 2);
  recorder.reset(RecorderOperationOwner::localApp, 8);
  const AudioCaptureDispatchResult localResult = dispatcher.drain(
      local, router, audio, recorder, voice, log, 10);
  assert(localResult.ok);
  assert(localResult.consumedFrames == 3);
  assert(recorder.acceptedFrames == 3);
  assertUnique(local.poppedSequences);

  FakeSource usb;
  usb.add(2, 40);
  usb.add(2, 41);
  recorder.reset(RecorderOperationOwner::linkUsb, 8);
  const AudioCaptureDispatchResult usbResult = dispatcher.drain(
      usb, router, audio, recorder, voice, log, 20);
  assert(usbResult.ok);
  assert(recorder.acceptedFrames == 2);
  assertUnique(usb.poppedSequences);

  FakeSource wifi;
  wifi.add(3, 90);
  recorder.reset(RecorderOperationOwner::linkWifi, 8);
  const AudioCaptureDispatchResult wifiResult = dispatcher.drain(
      wifi, router, audio, recorder, voice, log, 30);
  assert(wifiResult.ok);
  assert(recorder.acceptedFrames == 1);
  assertUnique(wifi.poppedSequences);
}

void runProductOrderOverflowAndSecondSession() {
  AudioCaptureDispatcher dispatcher;
  FakeRouter router(AudioCaptureOwner::localCapsule);
  FakeAudio audio;
  FakeRecorder recorder;
  FakeVoice voice;
  FakeLog log;

  // Product order: the App drains first. The Link poll must only observe the
  // recorder's durable failure fact; it cannot pop the same ring a second time.
  FakeSource firstSession;
  firstSession.add(10, 0);
  firstSession.add(10, 1);
  firstSession.add(10, 2);
  recorder.reset(RecorderOperationOwner::linkUsb, 1);
  const AudioCaptureDispatchResult appPoll = dispatcher.drain(
      firstSession, router, audio, recorder, voice, log, 100);
  assert(!appPoll.ok);
  assert(appPoll.recorderDeliveryFailure);
  assert(appPoll.firstFailure == AudioCaptureFailureCode::recorderDeliveryFailure);
  assert(appPoll.failedRecorderOwner == RecorderOperationOwner::linkUsb);
  assert(recorder.failureTerminal == RecorderTerminal::storageFailure);
  assert(recorder.failureStage ==
         RecorderFailureStage::storageQueueOverflow);
  assert(recorder.acceptedFrames == 1);
  assert(firstSession.poppedSequences.size() == 3);
  assertUnique(firstSession.poppedSequences);

  // Model the Link terminal settlement: failure publishes no Inbox item and
  // releases its owner before ACK allows a fresh session.
  const bool inboxPublished = recorder.failureStage == RecorderFailureStage::none;
  assert(!inboxPublished);
  recorder.active = false;
  recorder.owner = RecorderOperationOwner::none;

  FakeSource emptyLinkPoll;
  const AudioCaptureDispatchResult linkPoll = dispatcher.drain(
      emptyLinkPoll, router, audio, recorder, voice, log, 101);
  assert(!linkPoll.hadFrames);
  assert(firstSession.poppedSequences.size() == 3);

  FakeSource secondSession;
  secondSession.add(11, 0);
  secondSession.add(11, 1);
  recorder.reset(RecorderOperationOwner::linkUsb, 8);
  const AudioCaptureDispatchResult second = dispatcher.drain(
      secondSession, router, audio, recorder, voice, log, 200);
  assert(second.ok);
  assert(recorder.acceptedFrames == 2);
  assert(recorder.failureStage == RecorderFailureStage::none);
  assertUnique(secondSession.poppedSequences);
}

void runBleAndSequenceFailure() {
  AudioCaptureDispatcher dispatcher;
  FakeRouter router(AudioCaptureOwner::wirelessVoice);
  FakeAudio audio;
  FakeRecorder recorder;
  FakeVoice voice;
  FakeLog log;
  voice.accepting = true;
  recorder.active = false;
  recorder.owner = RecorderOperationOwner::none;

  FakeSource ble;
  ble.add(20, 5);
  ble.add(20, 6);
  const AudioCaptureDispatchResult bleResult = dispatcher.drain(
      ble, router, audio, recorder, voice, log, 300);
  assert(bleResult.ok);
  assert(voice.acceptedFrames == 2);

  router.set(AudioCaptureOwner::localCapsule);
  recorder.reset(RecorderOperationOwner::localApp, 8);
  FakeSource gap;
  gap.add(21, 0);
  gap.add(21, 2);
  const AudioCaptureDispatchResult gapResult = dispatcher.drain(
      gap, router, audio, recorder, voice, log, 400);
  assert(!gapResult.ok);
  assert(gapResult.sequenceIncomplete);
  assert(gapResult.recorderDeliveryFailure);
  assert(recorder.failureStage == RecorderFailureStage::captureIncomplete);
  assert(dispatcher.metrics().sequenceFailures == 1);
  assert(dispatcher.metrics().firstFailure ==
         AudioCaptureFailureCode::dispatchSequenceGap);
  assert(dispatcher.metrics().firstFailureSequence == 2);
  assert(dispatcher.metrics().maximumIntervalUs == 0);

  FakeSource later;
  later.add(21, 3);
  (void)dispatcher.drain(later, router, audio, recorder, voice, log, 420);
  assert(dispatcher.metrics().maximumIntervalUs == 20000U);
  assert(dispatcher.metrics().intervalSamples == 1U);
  assert(dispatcher.metrics().intervalHistogram[7] == 1U);
}

}  // namespace

int main() {
  runLocalAndLinkRouting();
  runProductOrderOverflowAndSecondSession();
  runBleAndSequenceFailure();
  return 0;
}
