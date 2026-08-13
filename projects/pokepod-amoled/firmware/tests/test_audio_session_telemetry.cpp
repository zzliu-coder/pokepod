#include <cassert>

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
  telemetry.observeCapture(capture, dispatcher, 700);
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
  telemetry.observeCapture(capture, dispatcher, 650);
  capture.ring.sessionId = 73;
  capture.ring.highWaterFrames = 1;
  dispatcher.sessionId = 73;
  telemetry.observeCapture(capture, dispatcher, 640);
  snapshot = telemetry.snapshot();
  assert(snapshot.sessionId == 73);
  assert(snapshot.captureRingHighWaterFrames == 1);
  assert(!snapshot.incomplete());
  return 0;
}
