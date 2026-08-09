#pragma once

#include <Arduino.h>

namespace pokepod {

class WirelessSyncPairingProvider {
 public:
  virtual ~WirelessSyncPairingProvider() = default;
  virtual bool pairingBundle(bool rotate, String &json, String &error) = 0;
};

}  // namespace pokepod
