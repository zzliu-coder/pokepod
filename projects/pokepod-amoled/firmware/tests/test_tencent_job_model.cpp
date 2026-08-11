#include <cassert>
#include <cstdint>

#include "../PokePodAmoled/TencentAsrControl.h"
#include "../PokePodAmoled/TencentJobModel.h"

using namespace pokepod;

namespace {

uint32_t randomState = 0x517cc1b7U;

uint32_t nextRandom() {
  randomState ^= randomState << 13;
  randomState ^= randomState >> 17;
  randomState ^= randomState << 5;
  return randomState;
}

}  // namespace

int main() {
  TencentCancelToken token;
  const uint32_t firstGeneration = token.begin();
  assert(!token.cancelled(firstGeneration));
  assert(token.cancel(firstGeneration));
  assert(token.cancelled(firstGeneration));
  const uint32_t secondGeneration = token.begin();
  assert(secondGeneration != firstGeneration);
  assert(!token.cancelled(secondGeneration));
  assert(!token.cancel(firstGeneration));
  assert(!token.cancelled(secondGeneration));

  std::atomic<uint8_t> asrStage{
      static_cast<uint8_t>(TencentAsrStage::idle)};
  TencentAsrControl control = {&token, secondGeneration, &asrStage};
  control.setStage(TencentAsrStage::hashing);
  assert(asrStage.load() == static_cast<uint8_t>(TencentAsrStage::hashing));

  TencentJobModel success;
  assert(success.queue(1, 180000));
  assert(success.state() == TencentJobState::queued);
  assert(success.start(1));
  assert(success.networkFinished(1, true, false));
  assert(success.state() == TencentJobState::committing);
  assert(!tencentJobIsTerminal(success.state()));
  assert(success.commitFinished(1, true));
  assert(success.state() == TencentJobState::succeeded);

  TencentJobModel commitFailure;
  assert(commitFailure.queue(9, 1000));
  assert(commitFailure.start(9));
  assert(commitFailure.networkFinished(9, true, false));
  assert(commitFailure.commitFinished(9, false));
  assert(commitFailure.state() == TencentJobState::failed);

  TencentJobModel cancelled;
  assert(cancelled.queue(10, 1000));
  assert(cancelled.start(10));
  assert(cancelled.cancel(10, TencentCancelReason::shutdown));
  assert(cancelled.state() == TencentJobState::cancelling);
  assert(!cancelled.networkFinished(9, true, false));
  assert(cancelled.networkFinished(10, true, false));
  assert(cancelled.state() == TencentJobState::cancelled);
  assert(!cancelled.commitFinished(10, true));

  TencentJobModel watchdog;
  assert(watchdog.queue(11, 300));
  assert(watchdog.start(11));
  assert(!watchdog.checkWatchdog(299));
  assert(watchdog.checkWatchdog(300));
  assert(watchdog.state() == TencentJobState::watchdog);
  assert(watchdog.cancelReason() == TencentCancelReason::watchdog);
  assert(watchdog.networkFinished(11, false, false));
  assert(watchdog.state() == TencentJobState::retryable);

  TencentJobModel wrappedDeadline;
  assert(wrappedDeadline.queue(12, 16));
  assert(wrappedDeadline.start(12));
  assert(!wrappedDeadline.checkWatchdog(0xfffffff0U));
  assert(wrappedDeadline.checkWatchdog(16));

  // Generations isolate a late result from the next capsule.
  assert(success.queue(2, 2000));
  assert(success.start(2));
  assert(!success.networkFinished(1, true, false));
  assert(success.state() == TencentJobState::working);

  // Property pass: arbitrary operations may never report success without
  // passing through the committing state for the current generation.
  TencentJobModel model;
  uint32_t generation = 100;
  bool sawCommitting = false;
  for (uint32_t step = 0; step < 100000; ++step) {
    const uint32_t value = nextRandom();
    const uint32_t selectedGeneration = (value & 7U) == 0
        ? generation - 1 : generation;
    switch ((value >> 3) % 7U) {
      case 0:
        if (model.queue(++generation, value | 1U)) sawCommitting = false;
        break;
      case 1: model.start(selectedGeneration); break;
      case 2:
        model.cancel(selectedGeneration, TencentCancelReason::maintenance);
        break;
      case 3:
        if (model.networkFinished(selectedGeneration, true, false) &&
            model.state() == TencentJobState::committing) {
          sawCommitting = true;
        }
        break;
      case 4:
        model.networkFinished(selectedGeneration, false, (value & 1U) != 0);
        break;
      case 5: model.checkWatchdog(value); break;
      case 6: model.commitFinished(selectedGeneration, (value & 1U) != 0); break;
    }
    if (model.state() == TencentJobState::succeeded) assert(sawCommitting);
    if (tencentJobIsTerminal(model.state())) {
      assert(!tencentJobOwnsResources(model.state()));
    }
  }
  return 0;
}
