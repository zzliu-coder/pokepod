#include <cassert>
#include <cstdint>

#include "ServiceQuiescencePolicy.h"

using namespace pokepod;

int main() {
  for (uint32_t mask = 0; mask < 64; ++mask) {
    ServiceQuiescenceFacts facts;
    facts.asrQuiesced = (mask & 1U) != 0;
    facts.captureBusy = (mask & 2U) != 0;
    facts.captureCompletionPending = (mask & 4U) != 0;
    facts.recorderCleanupPending = (mask & 8U) != 0;
    facts.playbackCleanupPending = (mask & 16U) != 0;
    facts.storageActive = (mask & 32U) != 0;
    assert(servicesQuiesced(facts) == (mask == 1U));
  }
  return 0;
}
