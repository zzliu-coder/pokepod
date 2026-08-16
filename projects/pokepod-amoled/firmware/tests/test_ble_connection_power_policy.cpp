#include <cassert>

#include "BleConnectionPowerPolicy.h"

using namespace pokepod;

int main() {
  static_assert(!bleFirmwareMayRequestConnectionParameters());
  const BleConnectionParameters idle =
      bleConnectionParameters(BleConnectionPowerMode::idle);
  const BleConnectionParameters voice =
      bleConnectionParameters(BleConnectionPowerMode::voice);
  assert(idle.minInterval == 24);
  assert(idle.maxInterval == 32);
  assert(idle.latency == 4);
  assert(voice.minInterval == 12);
  assert(voice.maxInterval == 16);
  assert(voice.latency == 0);
  assert(voice.maxInterval < idle.maxInterval);
  return 0;
}
