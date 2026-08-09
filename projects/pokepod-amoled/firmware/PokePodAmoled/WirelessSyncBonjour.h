#pragma once

#include <Arduino.h>

namespace pokepod {

class WirelessSyncBonjour {
 public:
  bool start(uint16_t port, bool paired, Print &log);
  void stop();
  bool active() const { return active_; }
  bool pairedAdvertised() const { return pairedAdvertised_; }
  const String &instanceNonce() const { return instanceNonce_; }

 private:
  bool active_ = false;
  bool pairedAdvertised_ = false;
  String instanceNonce_;
};

}  // namespace pokepod
