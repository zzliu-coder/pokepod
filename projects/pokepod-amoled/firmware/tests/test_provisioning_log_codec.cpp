#include <cassert>
#include <cstring>

#include "ProvisioningLogCodec.h"

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
  return 0;
}
