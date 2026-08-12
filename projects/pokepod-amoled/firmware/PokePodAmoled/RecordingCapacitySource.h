#pragma once

#include <stdint.h>

#include "RecordingAdmissionPolicy.h"

namespace pokepod {

// Platform seam for recorder admission. Production reads SD_MMC capacity and
// the monotonic ESP timer; host tests provide deterministic snapshots/timing.
// The recorder invokes query() exactly once while it owns both the mutation
// reservation and the matching physical IO lease.
class RecordingCapacitySource {
 public:
  virtual ~RecordingCapacitySource() = default;
  virtual RecordingSpaceSnapshot query() = 0;
  virtual uint64_t monotonicMicros() = 0;
};

}  // namespace pokepod
