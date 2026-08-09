#include <assert.h>

#include "BleVoiceQuality.h"

using namespace pokepod;

int main() {
  BleVoiceQualityCounters unresolved;
  unresolved.recordNotifyAttempt();
  const BleVoiceQualitySnapshot pending = unresolved.snapshot();
  assert(pending.notifyAttempts == 1);
  assert(pending.notifyAccepted == 0);
  assert(pending.notifyFailures == 0);

  BleVoiceQualityCounters counters;
  counters.recordNotifyAttempt();
  counters.recordNotifyStatus(true);
  counters.recordNotifyAttempt();
  counters.recordNotifyStatus(false);
  counters.recordSessionError(VoiceSessionError::readyTimeout);
  counters.recordSessionError(VoiceSessionError::queueOverflow);
  counters.recordSessionError(VoiceSessionError::stopAckTimeout);
  counters.recordSessionError(VoiceSessionError::streamTimeout);
  counters.recordSessionError(VoiceSessionError::none);
  const BleVoiceQualitySnapshot value = counters.snapshot();
  assert(value.notifyAttempts == 2);
  assert(value.notifyAccepted == 1);
  assert(value.notifyFailures == 1);
  assert(value.sessionFailures == 4);
  assert(value.readyTimeouts == 1);
  assert(value.queueOverflows == 1);
  assert(value.stopAckTimeouts == 1);
  assert(value.streamTimeouts == 1);
  assert(value.lastErrorCode ==
         static_cast<uint16_t>(VoiceSessionError::streamTimeout));
  assert(value.issueCount() == 5);
  return 0;
}
