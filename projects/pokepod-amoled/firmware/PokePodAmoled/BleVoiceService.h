#pragma once

#include <Arduino.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLEServer.h>

#if defined(CONFIG_NIMBLE_ENABLED) && \
    (!defined(CONFIG_BT_NIMBLE_MAX_CONNECTIONS) || \
     CONFIG_BT_NIMBLE_MAX_CONNECTIONS != 1)
#error "PokePod Voice requires a single NimBLE controller connection"
#endif

#include "AudioCaptureRouter.h"
#include "BlePeerPolicy.h"
#include "BleConnectionPowerPolicy.h"
#include "BleSingleConnectionPolicy.h"
#include "BleVoiceProtocol.h"
#include "BleVoiceQuality.h"
#include "VoiceSessionController.h"

namespace pokepod {

class BleVoiceService {
 public:
  bool begin(const String &deviceId, Print &log);
  void poll(uint32_t nowMs);

  bool startSession(uint32_t sessionId, uint32_t nowMs,
                    AudioCaptureRouter &router);
  void endSession();
  bool appendAudio(const uint8_t *stereo48, size_t bytes, uint32_t nowMs);
  void enterPairingMode(uint32_t nowMs);
  void cancelPairingMode();
  void forgetMac();
  void setBatteryPercent(int batteryPercent);
  bool pauseForIdleSleep();
  void resumeAfterIdleSleep();
  void prepareForDeepSleep();

  bool connected() const { return connected_; }
  bool idlePaused() const { return idlePaused_; }
  bool radioActive() const { return connected_ || !idlePaused_; }
  bool appReady() const { return connected_ && appReady_ && mtuReady(); }
  bool mtuReady() const { return bleVoiceMtuReady(mtu_); }
  uint16_t mtu() const { return mtu_; }
  BleVoiceQualitySnapshot quality() const;
  uint32_t sentFrames() const { return quality().notifyAccepted; }
  uint32_t sendFailures() const { return quality().notifyFailures; }
  uint32_t queueOverflows() const { return quality().queueOverflows; }
  VoiceSessionState sessionState() const { return controller_.state(); }
  VoiceSessionError sessionError() const { return controller_.error(); }
  bool streaming() const { return controller_.active(); }
  bool acceptingAudio() const { return controller_.acceptsAudio(); }
  uint32_t passkey() const { return passkey_; }
  bool bonded() const { return bonded_; }
  bool pairingMode(uint32_t nowMs) const {
    return static_cast<int32_t>(pairingUntilMs_ - nowMs) > 0;
  }
  bool securityAllowed(uint32_t nowMs) const {
    return connectionPolicy_.hasCurrent() &&
        peerPolicy_.securityAllowed(pairingMode(nowMs));
  }
  bool authorizationAllowed(uint16_t connectionId, uint32_t nowMs) const {
    return connectionPolicy_.isCurrent(connectionId) &&
        securityAllowed(nowMs);
  }
  uint16_t connectionIdForPeer(const uint8_t *peerAddress) const;

  void handleConnect(uint16_t connectionId, const uint8_t *peerAddress);
  void handleDisconnect(uint16_t connectionId);
  void handleMtu(uint16_t connectionId, uint16_t mtu);
  void handleCommand(uint16_t connectionId, const uint8_t *bytes,
                     size_t length);
  void handleAuthentication(uint16_t connectionId, bool success,
                            const uint8_t *peerAddress);
  void handleNotifyStatus(bool acceptedByHost);
  void handleControlNotifyStatus(bool acceptedByHost);
  void handleDeviceInfoRead();
  void handlePasskey(uint32_t passkey);

 private:
  bool notifyControl(BleVoiceEventType type, uint32_t sessionId,
                     uint16_t code = 0);
  void restartAdvertising();
  void activatePairingMode(uint32_t nowMs);
  void updateDeviceInfo();
  void requestConnectionPowerMode(BleConnectionPowerMode mode);

  BLEServer *server_ = nullptr;
  BLECharacteristic *info_ = nullptr;
  BLECharacteristic *command_ = nullptr;
  BLECharacteristic *event_ = nullptr;
  BLECharacteristic *audio_ = nullptr;
  BlePeerPolicy peerPolicy_;
  BleSingleConnectionPolicy connectionPolicy_;
  VoiceSessionController controller_;
  mutable portMUX_TYPE qualityMux_ = portMUX_INITIALIZER_UNLOCKED;
  BleVoiceQualityCounters quality_;
  Print *log_ = nullptr;
  String deviceId_;
  bool connected_ = false;
  bool authenticated_ = false;
  bool appReady_ = false;
  bool bonded_ = false;
  uint16_t connectionId_ = 0;
  uint8_t currentPeerAddress_[6] = {};
  bool currentPeerAddressValid_ = false;
  uint16_t mtu_ = 23;
  uint32_t passkey_ = 0;
  uint32_t pairingUntilMs_ = 0;
  volatile bool controlNotifyResolved_ = false;
  volatile bool controlNotifyAccepted_ = false;
  VoiceSessionError reportedError_ = VoiceSessionError::none;
  int batteryPercent_ = -1;
  BleConnectionPowerMode connectionPowerMode_ =
      BleConnectionPowerMode::idle;
  bool idlePaused_ = false;
};

}  // namespace pokepod
