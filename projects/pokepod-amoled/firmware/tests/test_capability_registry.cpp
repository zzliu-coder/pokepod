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
  capabilities.record(DeviceCapability::capsuleLibrary, true);
  capabilities.record(DeviceCapability::recording, false);
  assert(capabilities.observed(DeviceCapability::recording));
  assert(!capabilities.ready(DeviceCapability::recording));
  assert(!capabilities.allows(kRecordingCapabilities));

  capabilities.record(DeviceCapability::recording, true);
  capabilities.record(DeviceCapability::audio, true);
  assert(capabilities.allows(kRecordingCapabilities));
  assert(capabilities.allows(kCapsuleBrowsingCapabilities));

  StartupCapabilityPresentation startup =
      startupCapabilityPresentation(capabilities);
  assert(startup.mode ==
         StartupCapabilityMode::recordingWithoutTranscription);
  assert(startup.capsuleBrowsing);
  assert(startup.capsuleRecording);
  assert(!startup.transcription);

  capabilities.record(DeviceCapability::transcription, true);

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

  CapabilityRegistry wirelessOnly;
  wirelessOnly.record(DeviceCapability::storage, false);
  wirelessOnly.record(DeviceCapability::capsuleLibrary, false);
  wirelessOnly.record(DeviceCapability::recording, false);
  wirelessOnly.record(DeviceCapability::transcription, false);
  wirelessOnly.record(DeviceCapability::audio, true);
  wirelessOnly.record(DeviceCapability::bleVoice, true);
  startup = startupCapabilityPresentation(wirelessOnly);
  assert(startup.mode == StartupCapabilityMode::wirelessVoiceOnly);
  assert(!startup.capsuleBrowsing);
  assert(!startup.capsuleRecording);
  assert(startup.wirelessVoice);

  CapabilityRegistry browseOnly;
  browseOnly.record(DeviceCapability::storage, true);
  browseOnly.record(DeviceCapability::capsuleLibrary, true);
  browseOnly.record(DeviceCapability::recording, false);
  browseOnly.record(DeviceCapability::audio, true);
  startup = startupCapabilityPresentation(browseOnly);
  assert(startup.mode == StartupCapabilityMode::browsingOnly);
  assert(startup.capsuleBrowsing);
  assert(!startup.capsuleRecording);
  assert(!browseOnly.allows(kRecordingCapabilities));

  CapabilityRegistry unavailable;
  unavailable.record(DeviceCapability::storage, false);
  unavailable.record(DeviceCapability::capsuleLibrary, false);
  unavailable.record(DeviceCapability::audio, false);
  unavailable.record(DeviceCapability::recording, false);
  unavailable.record(DeviceCapability::bleVoice, false);
  startup = startupCapabilityPresentation(unavailable);
  assert(startup.mode == StartupCapabilityMode::unavailable);
  return 0;
}
