#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "AudioCaptureRing.h"
#include "AudioCaptureService.h"

using namespace pokepod;

namespace {

struct PlannedRead {
  AudioCaptureReadStatus status;
  size_t bytes;
  uint32_t elapsedUs;
};

class FakeCaptureSource final : public AudioCaptureSource {
 public:
  explicit FakeCaptureSource(std::vector<PlannedRead> plan)
      : plan_(plan) {}

  bool start() override {
    ++starts;
    active = startAllowed;
    return active;
  }

  void stop() override {
    ++stops;
    active = false;
  }

  AudioCaptureReadResult readStereo48(uint8_t *output, size_t capacity,
                                      uint32_t timeoutMs) override {
    assert(active);
    assert(timeoutMs == 25);
    if (next_ >= plan_.size()) {
      return {AudioCaptureReadStatus::timeout, 0, timeoutMs * 1000};
    }
    const PlannedRead item = plan_[next_++];
    size_t writable = item.bytes < capacity ? item.bytes : capacity;
    writable -= writable % 4;
    for (size_t offset = 0; offset + 3 < writable; offset += 4) {
      const int16_t left = static_cast<int16_t>(
          4000 + static_cast<int16_t>((sample_++ % 64) * 80));
      output[offset] = static_cast<uint8_t>(left);
      output[offset + 1] = static_cast<uint8_t>(left >> 8);
      output[offset + 2] = 0;
      output[offset + 3] = 0;
    }
    return {item.status, item.bytes, item.elapsedUs};
  }

  bool startAllowed = true;
  bool active = false;
  uint32_t starts = 0;
  uint32_t stops = 0;

 private:
  std::vector<PlannedRead> plan_;
  size_t next_ = 0;
  uint32_t sample_ = 0;
};

void fillSamples(int16_t *samples, int16_t base) {
  for (size_t index = 0; index < kAudioCaptureSamplesPerFrame; ++index) {
    samples[index] = static_cast<int16_t>(base + index);
  }
}

}  // namespace

int main() {
  static_assert(kAudioCaptureFrameDurationMs == 20, "wire cadence changed");
  static_assert(kAudioCaptureSamplesPerFrame == 320, "frame size changed");
  static_assert(AudioCaptureService<6>::kRawStereoBytesPerFrame == 3840,
                "48 kHz stereo input block changed");

  AudioCaptureRing<2> ring;
  ring.resetSession(41);
  int16_t first[kAudioCaptureSamplesPerFrame];
  int16_t second[kAudioCaptureSamplesPerFrame];
  fillSamples(first, 100);
  fillSamples(second, 200);
  assert(ring.push(0xfffffffeU, 10, first, kAudioCaptureSamplesPerFrame));
  assert(ring.push(0xffffffffU, 30, second, kAudioCaptureSamplesPerFrame));
  assert(!ring.push(0, 50, first, kAudioCaptureSamplesPerFrame));
  assert(!ring.push(0, 50, first, kAudioCaptureSamplesPerFrame - 1));
  AudioCaptureRingMetrics ringFull = ring.metrics();
  assert(ringFull.currentFrames == 2);
  assert(ringFull.highWaterFrames == 2);
  assert(ringFull.pushedFrames == 2);
  assert(ringFull.droppedFrames == 1);
  assert(ringFull.droppedSamples == 320);
  assert(ringFull.incomplete());

  AudioCaptureFrame frame;
  assert(ring.pop(frame));
  assert(frame.sessionId == 41 && frame.sequence == 0xfffffffeU);
  assert(frame.capturedAtMs == 10 && frame.samples[3] == 103);
  assert(ring.pop(frame));
  assert(frame.sequence == 0xffffffffU && frame.samples[3] == 203);
  assert(!ring.pop(frame));
  assert(ring.metrics().poppedFrames == 2);

  // Session reset is explicit and may only happen while producer/consumer are
  // stopped. It clears stale frames and per-session loss evidence.
  ring.resetSession(42);
  assert(ring.size() == 0);
  assert(ring.metrics().sessionId == 42);
  assert(!ring.metrics().incomplete());

  const size_t raw = AudioCaptureService<2>::kRawStereoBytesPerFrame;
  FakeCaptureSource source({
      {AudioCaptureReadStatus::ok, raw / 2, 5000},
      {AudioCaptureReadStatus::ok, raw / 2, 20000},
      {AudioCaptureReadStatus::overrun, raw, 50000},
      {AudioCaptureReadStatus::ok, raw, 4000},
      {AudioCaptureReadStatus::ok, raw, 4000},
      {AudioCaptureReadStatus::timeout, 0, 25000},
      {AudioCaptureReadStatus::failure, 0, 1000},
  });
  AudioCaptureService<2> service;
  assert(service.startSession(77, source));
  assert(!service.startSession(78, source));
  assert(service.captureOnce(1) == AudioCaptureCycleResult::partialInput);
  // The first complete raw block primes channel selection and FIR state.
  assert(service.captureOnce(2) == AudioCaptureCycleResult::partialInput);
  AudioCaptureCycleResult result = service.captureOnce(22);
  assert(result == AudioCaptureCycleResult::frameQueued);
  result = service.captureOnce(42);
  assert(result == AudioCaptureCycleResult::frameQueued);
  // The fixed two-frame ring refuses the next frame and records exact loss.
  result = service.captureOnce(62);
  assert(result == AudioCaptureCycleResult::frameDropped);
  AudioCaptureServiceMetrics metrics = service.metrics();
  assert(metrics.ring.sessionId == 77);
  assert(metrics.ring.currentFrames == 2);
  assert(metrics.ring.highWaterFrames == 2);
  assert(metrics.ring.droppedFrames == 1);
  assert(metrics.ring.droppedSamples == 320);
  assert(metrics.sourceOverruns == 1);
  assert(metrics.shortReads == 1);
  assert(metrics.longestReadUs == 50000);

  assert(service.pop(frame));
  assert(frame.sessionId == 77 && frame.sequence == 0);
  assert(service.pop(frame));
  assert(frame.sequence == 1);
  assert(!service.pop(frame));
  assert(service.captureOnce(82) == AudioCaptureCycleResult::sourceTimeout);
  assert(service.captureOnce(102) == AudioCaptureCycleResult::sourceFailure);
  metrics = service.metrics();
  assert(metrics.timeouts == 1);
  assert(metrics.sourceFailures == 1);
  service.stopSession();
  assert(!service.running());
  assert(source.starts == 1 && source.stops == 1);
  assert(service.captureOnce(122) == AudioCaptureCycleResult::idle);

  FakeCaptureSource refused({});
  refused.startAllowed = false;
  AudioCaptureService<> refusedService;
  assert(!refusedService.startSession(88, refused));
  assert(!refusedService.running());
  return 0;
}
