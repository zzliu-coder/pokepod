#include <cassert>
#include <cstdint>

#include "WirelessCaptureStartPolicy.h"

using namespace pokepod;

int main() {
  WirelessCaptureStartPolicy policy;
  AudioCaptureServiceMetrics metrics;
  assert(policy.begin(42, 0xfffffff0U));
  metrics.ring.sessionId = 42;
  assert(policy.observe(0xfffffff5U, metrics) ==
         WirelessCaptureStartAction::waiting);
  metrics.ring.currentFrames = 1;
  metrics.ring.pushedFrames = 1;
  assert(policy.observe(10, metrics) ==
         WirelessCaptureStartAction::startBleSession);
  policy.finish();

  assert(policy.begin(43, 100));
  metrics = {};
  metrics.ring.sessionId = 43;
  metrics.firstFailure = AudioCaptureFailureCode::earlyZeroRead;
  assert(policy.observe(101, metrics) ==
         WirelessCaptureStartAction::abortCapture);
  policy.finish();

  assert(policy.begin(44, 1000));
  metrics = {};
  metrics.ring.sessionId = 44;
  assert(policy.observe(1499, metrics) ==
         WirelessCaptureStartAction::waiting);
  assert(policy.observe(1500, metrics) ==
         WirelessCaptureStartAction::abortCapture);
  policy.finish();
  return 0;
}
