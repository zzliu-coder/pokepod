#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "WirelessSyncIdentityBlob.h"
#include "WirelessSyncPairing.h"

namespace pokepod {

class WirelessSyncIdentity : public WirelessSyncPairingProvider {
 public:
  bool begin(uint64_t hardwareId, Print &log);
  bool pairingBundle(bool rotate, String &json, String &error) override;

  bool ready() const { return ready_; }
  bool paired() const { return ready_ && stored_.paired != 0; }
  const char *pairingId() const { return stored_.pairingId; }
  const uint8_t *secret() const { return stored_.secret; }
  const uint8_t *certificate() const { return stored_.certificate; }
  size_t certificateBytes() const { return stored_.certificateBytes; }
  const uint8_t *privateKey() const { return stored_.privateKey; }
  size_t privateKeyBytes() const { return stored_.privateKeyBytes; }
  const char *deviceId() const { return deviceId_; }
  const char *certificateSha256() const { return certificateSha256_; }

 private:
  bool generate();
  bool rotatePairing();
  bool persist();
  bool computeCertificateFingerprint();

  Preferences preferences_;
  StoredWirelessSyncIdentity stored_;
  char deviceId_[24] = {};
  char certificateSha256_[65] = {};
  bool preferencesOpen_ = false;
  bool ready_ = false;
  Print *log_ = nullptr;
};

}  // namespace pokepod
