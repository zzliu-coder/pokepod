#include <cassert>
#include <cstring>

#include "ProvisioningLogCodec.h"
#include "ProvisioningProbeCodec.h"

using namespace pokepod;

int main() {
  const char vector[] = "123456789";
  assert(provisioningLogCrc32(
      reinterpret_cast<const uint8_t *>(vector), 9) == 0xcbf43926U);

  StoredProvisioningLog log{};
  initializeProvisioningLog(log);
  finalizeProvisioningLog(log);
  assert(validateProvisioningLog(log));
  assert(provisioningLogNewest(log, 0) == nullptr);

  for (size_t index = 0; index < kProvisioningLogCapacity + 3; ++index) {
    StoredProvisioningLogRecord record{};
    record.stage = static_cast<uint8_t>(ProvisioningLogStage::connectStarted);
    record.outcome = static_cast<uint8_t>(ProvisioningLogOutcome::info);
    std::strcpy(record.ssid, "Liu");
    appendProvisioningLog(log, record);
  }
  finalizeProvisioningLog(log);
  assert(validateProvisioningLog(log));
  assert(log.count == kProvisioningLogCapacity);
  assert(provisioningLogNewest(log, 0)->sequence ==
         kProvisioningLogCapacity + 3);
  assert(provisioningLogNewest(log, kProvisioningLogCapacity - 1)->sequence == 4);

  StoredProvisioningLog corrupt = log;
  corrupt.records[0].ssid[0] ^= 1;
  assert(!validateProvisioningLog(corrupt));

  StoredProvisioningLog future = log;
  future.version += 1;
  finalizeProvisioningLog(future);
  assert(!validateProvisioningLog(future));

  StoredProvisioningLog unterminated = log;
  std::memset(unterminated.records[0].ssid, 'x',
              sizeof(unterminated.records[0].ssid));
  finalizeProvisioningLog(unterminated);
  assert(!validateProvisioningLog(unterminated));

  StoredProvisioningLog requested{};
  initializeProvisioningLog(requested);
  StoredProvisioningLogRecord request{};
  request.stage = static_cast<uint8_t>(ProvisioningLogStage::portalRequested);
  request.outcome = static_cast<uint8_t>(ProvisioningLogOutcome::info);
  appendProvisioningLog(requested, request);
  finalizeProvisioningLog(requested);
  assert(validateProvisioningLog(requested));

  StoredProvisioningLog staged = requested;
  StoredProvisioningLogRecord radio{};
  radio.stage = static_cast<uint8_t>(ProvisioningLogStage::radioModeStarted);
  radio.outcome = static_cast<uint8_t>(ProvisioningLogOutcome::success);
  appendProvisioningLog(staged, radio);
  StoredProvisioningLogRecord accessPoint{};
  accessPoint.stage =
      static_cast<uint8_t>(ProvisioningLogStage::accessPointStarted);
  accessPoint.outcome = static_cast<uint8_t>(ProvisioningLogOutcome::success);
  appendProvisioningLog(staged, accessPoint);
  finalizeProvisioningLog(staged);
  assert(validateProvisioningLog(staged));

  StoredProvisioningProbe probe{};
  initializeProvisioningProbe(probe);
  probe.stage = static_cast<uint8_t>(ProvisioningProbeStage::beforeModeAp);
  probe.sequence = 19;
  probe.internalFree = 120000;
  probe.internalLargest = 64000;
  probe.psramFree = 7000000;
  probe.psramLargest = 6000000;
  probe.psramTotal = 8388608;
  finalizeProvisioningProbe(probe);
  assert(validateProvisioningProbe(probe));
  assert(std::strcmp(provisioningProbeStageKey(
      ProvisioningProbeStage::beforePageSend), "before_page_send") == 0);

  StoredProvisioningProbe badProbe = probe;
  badProbe.psramFree ^= 1;
  assert(!validateProvisioningProbe(badProbe));

  StoredProvisioningProbe futureProbe = probe;
  ++futureProbe.version;
  finalizeProvisioningProbe(futureProbe);
  assert(!validateProvisioningProbe(futureProbe));

  StoredProvisioningProbe badStage = probe;
  badStage.stage = 255;
  finalizeProvisioningProbe(badStage);
  assert(!validateProvisioningProbe(badStage));
  return 0;
}
