#include <cassert>
#include <atomic>
#include <thread>

#include "AudioSessionTelemetry.h"

using namespace pokepod;

int main() {
  AudioSessionTelemetry telemetry;
  telemetry.reset();
  const uint32_t firstGeneration = telemetry.snapshot().generation;
  assert(firstGeneration != 0);

  AudioCaptureServiceMetrics capture;
  capture.ring.sessionId = 71;
  capture.ring.highWaterFrames = 5;
  capture.ring.droppedFrames = 1;
  capture.timeouts = 2;
  capture.longestReadUs = 49000;
  capture.zeroByteReads = 3;
  capture.earlyZeroReads = 1;
  capture.sourceOverruns = 1;
  capture.sourceFailures = 4;
  capture.sourceOverrunObservable = true;
  AudioCaptureDispatcherMetrics dispatcher;
  dispatcher.sessionId = 71;
  dispatcher.sequenceFailures = 1;
  dispatcher.maximumIntervalUs = 18000U;
  dispatcher.intervalHistogram[5] = 99;
  dispatcher.intervalHistogram[6] = 1;
  AudioSessionTelemetryProducer producer = telemetry.bindCaptureSession(71);
  telemetry.observeCapture(producer, capture, dispatcher, 700);
  telemetry.observeRecorderQueue(96, 1, 900);
  for (uint32_t i = 0; i < 998; ++i) telemetry.recordStorageWrite(3000U);
  telemetry.recordStorageWrite(9000U);
  telemetry.recordStorageWrite(40000U);
  telemetry.freeze();

  AudioSessionTelemetrySnapshot snapshot = telemetry.snapshot();
  assert(snapshot.sessionId == 71);
  assert(snapshot.captureRingHighWaterFrames == 5);
  assert(snapshot.captureRingDroppedFrames == 1);
  assert(snapshot.recorderQueueHighWaterFrames == 96);
  assert(snapshot.recorderQueueDroppedFrames == 1);
  assert(snapshot.dispatcherMaximumIntervalUs == 18000U);
  assert(snapshot.dispatcherP99IntervalUs == 8000U);
  assert(snapshot.i2sTimeouts == 2);
  assert(snapshot.i2sLongestReadUs == 49000);
  assert(snapshot.zeroByteReads == 3);
  assert(snapshot.earlyZeroReads == 1);
  assert(snapshot.sourceFailures == 4);
  assert(snapshot.sequenceGaps == 1);
  assert(snapshot.storageWriteP99Us == 4000U);
  assert(snapshot.storageWriteP999Us == 16000U);
  assert(snapshot.captureTaskStackHighWaterWords == 700);
  assert(snapshot.recorderTaskStackHighWaterWords == 900);
  assert(snapshot.sourceOverrunObservable);
  assert(snapshot.incomplete());
  assert(snapshot.frozen);

  telemetry.observeRecorderQueue(120, 4, 500);
  assert(telemetry.snapshot().recorderQueueHighWaterFrames == 96);
  telemetry.reset();
  snapshot = telemetry.snapshot();
  assert(snapshot.generation != firstGeneration);
  assert(snapshot.sessionId == 0);
  assert(!snapshot.frozen);
  assert(!snapshot.incomplete());

  capture.ring.sessionId = 72;
  capture.ring.highWaterFrames = 2;
  capture.ring.droppedFrames = 0;
  capture.timeouts = 0;
  capture.zeroByteReads = 0;
  capture.earlyZeroReads = 0;
  capture.sourceOverruns = 0;
  capture.sourceFailures = 0;
  dispatcher = {};
  dispatcher.sessionId = 72;
  producer = telemetry.bindCaptureSession(72);
  telemetry.observeCapture(producer, capture, dispatcher, 650);
  capture.ring.sessionId = 73;
  capture.ring.highWaterFrames = 1;
  dispatcher.sessionId = 73;
  producer = telemetry.bindCaptureSession(73);
  telemetry.observeCapture(producer, capture, dispatcher, 640);
  snapshot = telemetry.snapshot();
  assert(snapshot.sessionId == 73);
  assert(snapshot.captureRingHighWaterFrames == 1);
  assert(!snapshot.incomplete());

  // Reset racing live observations cannot advance the generation. A terminal
  // freeze is the ownership boundary that permits the next reset.
  {
    AudioSessionTelemetry resetRace;
    resetRace.reset();
    const uint32_t resetRaceGeneration = resetRace.snapshot().generation;
    const AudioSessionTelemetryProducer resetRaceProducer =
        resetRace.bindCaptureSession(91);
    AudioCaptureServiceMetrics resetCapture;
    resetCapture.ring.sessionId = 91;
    resetCapture.ring.highWaterFrames = 6;
    AudioCaptureDispatcherMetrics resetDispatcher;
    resetDispatcher.sessionId = 91;
    std::atomic<bool> startResetRace{false};
    std::thread resetter([&]() {
      while (!startResetRace.load(std::memory_order_acquire)) {}
      for (uint32_t i = 0; i < 50000U; ++i) resetRace.reset();
    });
    std::thread observer([&]() {
      while (!startResetRace.load(std::memory_order_acquire)) {}
      for (uint32_t i = 0; i < 50000U; ++i) {
        resetRace.observeCapture(resetRaceProducer, resetCapture,
                                 resetDispatcher, 600);
      }
    });
    startResetRace.store(true, std::memory_order_release);
    resetter.join();
    observer.join();
    const AudioSessionTelemetrySnapshot live = resetRace.snapshot();
    assert(live.generation == resetRaceGeneration);
    assert(live.sessionId == 91);
    assert(live.captureRingHighWaterFrames == 6);
    resetRace.freeze();
    resetRace.reset();
    assert(resetRace.snapshot().generation != resetRaceGeneration);
  }

  // Writers serialize through the telemetry publication sequence. Readers
  // may observe an older or newer generation, but never a mixture of both.
  AudioSessionTelemetry concurrent;
  for (uint32_t round = 1; round <= 50000U; ++round) {
    concurrent.reset();
    const uint32_t generation = concurrent.snapshot().generation;
    const uint32_t session = round + 100U;
    const AudioSessionTelemetryProducer concurrentProducer =
        concurrent.bindCaptureSession(session);
    std::atomic<bool> start{false};
    std::thread captureWriter([&]() {
      AudioCaptureServiceMetrics concurrentCapture;
      concurrentCapture.ring.sessionId = session;
      concurrentCapture.ring.highWaterFrames = 4;
      concurrentCapture.ring.droppedFrames = 2;
      concurrentCapture.longestReadUs = 43210;
      AudioCaptureDispatcherMetrics concurrentDispatcher;
      concurrentDispatcher.sessionId = session;
      concurrentDispatcher.sequenceFailures = 3;
      while (!start.load(std::memory_order_acquire)) {}
      concurrent.observeCapture(concurrentProducer, concurrentCapture,
                                concurrentDispatcher, 512);
    });
    std::thread recorderWriter([&]() {
      while (!start.load(std::memory_order_acquire)) {}
      concurrent.observeRecorderQueue(96, 1, 768);
      concurrent.recordStorageWrite(7000);
    });
    std::thread freezer([&]() {
      while (!start.load(std::memory_order_acquire)) {}
      concurrent.freeze();
    });
    start.store(true, std::memory_order_release);
    AudioSessionTelemetrySnapshot observed = concurrent.snapshot();
    assert(observed.generation == generation);
    if (observed.sessionId != 0) {
      assert(observed.sessionId == session);
      if (observed.captureRingHighWaterFrames != 0) {
        assert(observed.captureRingHighWaterFrames == 4);
        assert(observed.captureRingDroppedFrames == 2);
        assert(observed.i2sLongestReadUs == 43210);
        assert(observed.sequenceGaps == 3);
      }
    }
    captureWriter.join();
    recorderWriter.join();
    freezer.join();
    const AudioSessionTelemetrySnapshot terminal = concurrent.snapshot();
    assert(terminal.generation == generation);
    assert(terminal.frozen);
    concurrent.observeRecorderQueue(127, 99, 1);
    concurrent.recordStorageWrite(UINT32_MAX);
    const AudioSessionTelemetrySnapshot immutable = concurrent.snapshot();
    assert(immutable.generation == terminal.generation);
    assert(immutable.sessionId == terminal.sessionId);
    assert(immutable.captureRingHighWaterFrames ==
           terminal.captureRingHighWaterFrames);
    assert(immutable.recorderQueueHighWaterFrames ==
           terminal.recorderQueueHighWaterFrames);
    assert(immutable.storageWriteP999Us == terminal.storageWriteP999Us);
    concurrent.reset();
    concurrent.observeCapture(concurrentProducer, capture, dispatcher, 1);
    assert(concurrent.snapshot().sessionId == 0);
  }
  return 0;
}
