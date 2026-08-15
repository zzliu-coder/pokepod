#include <cassert>
#include <cstdint>

#include "RuntimeDiagnosticsCodec.h"

using namespace pokepod;

int main() {
  static_assert(sizeof(StoredRuntimeDiagnosticRecord) == 38);
  static_assert(sizeof(StoredRuntimeDiagnosticLog) < 1024);

  StoredRuntimeDiagnosticLog log{};
  initializeRuntimeDiagnosticLog(log);
  finalizeRuntimeDiagnosticLog(log);
  assert(validateRuntimeDiagnosticLog(log));
  assert(log.count == 0);

  for (uint32_t index = 0; index < kRuntimeDiagnosticsCapacity + 3; ++index) {
    StoredRuntimeDiagnosticRecord record{};
    record.subsystem = static_cast<uint8_t>(RuntimeDiagnosticSubsystem::recording);
    record.stage = static_cast<uint8_t>(RuntimeDiagnosticStage::recordingProbeWrite);
    record.outcome = static_cast<uint8_t>(RuntimeDiagnosticOutcome::started);
    record.detail0 = index;
    appendRuntimeDiagnostic(log, record);
  }
  finalizeRuntimeDiagnosticLog(log);
  assert(validateRuntimeDiagnosticLog(log));
  assert(log.count == kRuntimeDiagnosticsCapacity);
  assert(runtimeDiagnosticNewest(log, 0)->detail0 ==
         kRuntimeDiagnosticsCapacity + 2);
  assert(runtimeDiagnosticNewest(log, kRuntimeDiagnosticsCapacity - 1)->detail0 == 3);

  log.records[0].detail0 ^= 1U;
  assert(!validateRuntimeDiagnosticLog(log));
  assert(runtimeDiagnosticStageKey(RuntimeDiagnosticStage::recordingProbeFlush) != nullptr);
  assert(runtimeDiagnosticOutcomeKey(RuntimeDiagnosticOutcome::failure) != nullptr);
  return 0;
}
