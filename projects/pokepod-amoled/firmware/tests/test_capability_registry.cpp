#include <cassert>
#include <cstdint>
#include <initializer_list>

#include "CapabilityRegistry.h"

using namespace pokepod;

int main() {
  CapabilityRegistry capabilities;
  assert(!capabilities.allReady());
  assert(!capabilities.allows(kRecordingCapabilities));

  capabilities.record(DeviceCapability::storage, true);
  capabilities.record(DeviceCapability::recording, false);
  assert(capabilities.observed(DeviceCapability::recording));
  assert(!capabilities.ready(DeviceCapability::recording));
  assert(!capabilities.allows(kRecordingCapabilities));

  capabilities.record(DeviceCapability::recording, true);
  capabilities.record(DeviceCapability::audio, true);
  assert(capabilities.allows(kRecordingCapabilities));

  for (DeviceCapability capability : {
           DeviceCapability::display, DeviceCapability::touch,
           DeviceCapability::bleVoice, DeviceCapability::link,
           DeviceCapability::wifi, DeviceCapability::rtc,
           DeviceCapability::imu, DeviceCapability::pmu}) {
    capabilities.record(capability, true);
  }
  assert(capabilities.allReady());
  assert(capabilities.missingMask() == 0);

  capabilities.record(DeviceCapability::wifi, false);
  assert(!capabilities.allReady());
  assert(!capabilities.allows(kComputerSyncCapabilities));
  assert(capabilities.ready(DeviceCapability::link));
  assert((capabilities.missingMask() &
          capabilityMask(DeviceCapability::wifi)) != 0);
  return 0;
}
