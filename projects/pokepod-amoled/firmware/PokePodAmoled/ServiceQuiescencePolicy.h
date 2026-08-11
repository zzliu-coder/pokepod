#pragma once

namespace pokepod {

// Single shutdown/unmount gate shared by deep sleep and safe power-off.
// Every fact is a live ownership fact; presentation state is intentionally
// excluded so UI cannot make SD unmount decisions.
struct ServiceQuiescenceFacts {
  bool asrQuiesced = false;
  bool captureBusy = false;
  bool captureCompletionPending = false;
  bool recorderCleanupPending = false;
  bool playbackCleanupPending = false;
  bool storageActive = false;
};

inline bool servicesQuiesced(const ServiceQuiescenceFacts &facts) {
  return facts.asrQuiesced && !facts.captureBusy &&
      !facts.captureCompletionPending && !facts.recorderCleanupPending &&
      !facts.playbackCleanupPending && !facts.storageActive;
}

}  // namespace pokepod
