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
#include "BleAppHandshakePolicy.h"
#include "BlePeerPolicy.h"
#include "BleConnectionPowerPolicy.h"
#include "BleCallbackOverflowPolicy.h"
#include "BleVoiceCallbackMailbox.h"
#include "BleSingleConnectionPolicy.h"
#include "BleServiceEnablePolicy.h"
#include "BleServiceCallbackGate.h"
#include "BleVoiceProtocol.h"
#include "BleVoiceQuality.h"
#include "BleNotifyReliability.h"
#include "VoiceSessionController.h"

namespace pokepod {

class BleVoiceService {
 public:
  bool begin(const String &deviceId, bool userEnabled, Print &log);
  void poll(uint32_t nowMs);
  void requestEnable();
  void requestDisable(uint32_t nowMs);
  bool sessionStopRequested() const {
    return sessionStopRequest_.requested();
  }
  void acknowledgeSessionStopRequest() {
    sessionStopRequest_.acknowledge();
  }

  bool startSession(uint32_t sessionId, uint32_t nowMs,
                    AudioCaptureRouter &router);
  void endSession();
  bool appendAudio(const uint8_t *stereo48, size_t bytes, uint32_t nowMs);
  bool appendMono16(const int16_t *samples, size_t count, uint32_t nowMs);
  void abortSession(VoiceSessionError error);
  void enterPairingMode(uint32_t nowMs);
  void cancelPairingMode();
  void forgetMac();
  void setBatteryPercent(int batteryPercent);
  bool pauseForIdleSleep();
  void resumeAfterIdleSleep();
  void prepareForDeepSleep();

  bool connected() const { return connected_; }
  bool userEnabled() const { return enablePolicy_.userEnabled(); }
  bool disablePending() const { return enablePolicy_.transitionPending(); }
  bool idlePaused() const { return idlePaused_; }
  bool radioActive() const {
    return !quiescedForSleep() ||
        (userEnabled() && !idlePaused_);
  }
  bool quiescedForSleep() const {
    return bleVoiceQuiescedForSleep(sleepQuiescenceFacts());
  }
  bool callbackOverflowHardFailed() const {
    return callbackOverflow_.hardFailed();
  }
  bool callbackOverflowRecoveryRequired() const {
    return callbackOverflow_.requiresProcessRecovery();
  }
  bool appHandshakeDisconnectPending() const {
    return appHandshake_.disconnectPending();
  }
  bool appHandshakeRecoveryRequired() const {
    return appHandshake_.recoveryRequired();
  }
  bool claimAppHandshakeRecoveryRestart() {
    if (!appHandshakeRecoveryRequired() ||
        appHandshakeRecoveryRestartClaimed_) {
      return false;
    }
    appHandshakeRecoveryRestartClaimed_ = true;
    return true;
  }
  bool claimCallbackOverflowRecoveryRestart() {
    if (!callbackOverflowRecoveryRequired() ||
        callbackOverflowRecoveryRestartClaimed_) {
      return false;
    }
    callbackOverflowRecoveryRestartClaimed_ = true;
    return true;
  }
  uint16_t callbackOverflowAttempts() const {
    return callbackOverflow_.attempts();
  }
  uint32_t callbackOverflowHardFailures() const {
    return callbackOverflow_.hardFailureCount();
  }
  bool appReady() const {
    return enablePolicy_.acceptsNewWork() && !callbackOverflow_.active() &&
        !appHandshake_.disconnectPending() && connected_ && appReady_ &&
        mtuReady();
  }
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
  bool callbackSecurityAllowed() const {
    return callbackSecurity_.securityAllowed();
  }
  bool callbackAuthorizationAllowed(uint16_t connectionId) const {
    return callbackSecurity_.authorizationAllowed(connectionId);
  }
  uint32_t callbackPasskey() const {
    return callbackSecurity_.passkey();
  }
  uint16_t connectionIdForPeer(const uint8_t *peerAddress) const;

  void handleConnect(uint16_t connectionId, const uint8_t *peerAddress,
                     bool peerBonded = false);
  void handleDisconnect(uint16_t connectionId);
  void handleMtu(uint16_t connectionId, uint16_t mtu);
  void handleCommand(uint16_t connectionId, const uint8_t *bytes,
                     size_t length);
  void handleAuthentication(uint16_t connectionId, bool success,
                            const uint8_t *peerAddress);
  void handleNotifyStatus(bool acceptedByHost, uintptr_t callbackTask);
  void handleControlNotifyStatus(bool acceptedByHost,
                                 uintptr_t callbackTask);
  void handleDeviceInfoRead();
  void handlePasskey(uint32_t passkey);

 private:
  enum class ControlNotifyPurpose : uint8_t {
    none,
    generic,
    sessionStart,
    sessionEnd,
    error,
  };

  bool notifyControl(BleVoiceEventType type, uint32_t sessionId,
                     uint16_t code = 0);
  bool publishCallbackEvent(const BleVoiceCallbackEvent &event);
  void drainCallbackEvents(uint32_t nowMs);
  void drainNotifyStatusEvents(uint32_t nowMs);
  void processCallbackEvent(const BleVoiceCallbackEvent &event,
                            uint32_t nowMs);
  void processConnect(uint16_t connectionId, uint32_t connectionGeneration,
                      const uint8_t *peerAddress, bool peerBonded,
                      uint32_t nowMs);
  void processDisconnect(uint16_t connectionId,
                         uint32_t connectionGeneration, uint32_t nowMs);
  void processMtu(uint16_t connectionId, uint32_t connectionGeneration,
                  uint16_t mtu);
  void processCommand(uint16_t connectionId, const uint8_t *bytes,
                      size_t length, uint32_t connectionGeneration,
                      uint32_t nowMs);
  void processAuthentication(uint16_t connectionId, bool success,
                             const uint8_t *peerAddress,
                             uint32_t connectionGeneration,
                             uint32_t nowMs);
  void processNotifyStatus(const BleVoiceCallbackEvent &event);
  void processControlNotifyStatus(const BleVoiceCallbackEvent &event,
                                  uint32_t nowMs);
  void processDeviceInfoRead();
  void processPasskey(uint32_t passkey);
  void failClosedCallbackOverflow(uint32_t nowMs,
                                  const BleVoiceConnectionEpoch &epoch);
  void advanceCallbackOverflow(uint32_t nowMs);
  bool finishCallbackOverflowIfDisconnected(uint32_t nowMs);
  bool recoverInvalidCallbackOverflow(uint32_t nowMs);
  void refreshCallbackSnapshot(uint32_t nowMs);
  void clearControlNotify();
  void restartAdvertising();
  void activatePairingMode(uint32_t nowMs);
  void updateDeviceInfo();
  void requestConnectionPowerMode(BleConnectionPowerMode mode);
  void resetAudioNotify();
  void failAudioNotify(VoiceSessionError error);
  BleVoiceNotifyIdentity nextNotifyIdentity(BleVoiceNotifyKind kind);
  bool beginNotifyCallback(const BleVoiceNotifyIdentity &identity);
  void endNotifyCallback();
  void applyEnableActions(const BleServiceEnableActions &actions,
                          uint32_t nowMs);
  void clearDisabledRuntime(uint32_t nowMs);
  bool physicalConnectionPending() const {
    return connected_ || connectionPolicy_.hasCurrent() ||
        callbackSecurity_.connectionId() != kInvalidBleConnectionId ||
        callbackOverflow_.physicalConnectionPending();
  }
  uint16_t physicalConnectionId() const {
    if (callbackOverflow_.physicalConnectionPending()) {
      return callbackOverflow_.epoch().connectionId;
    }
    if (connectionPolicy_.hasCurrent()) {
      return connectionPolicy_.currentConnectionId();
    }
    return callbackSecurity_.connectionId();
  }
  BleSleepQuiescenceFacts sleepQuiescenceFacts() const {
    BleSleepQuiescenceFacts facts;
    facts.sessionActive = controller_.active();
    facts.pairingActive = pairingUntilMs_ != 0;
    facts.enableTransitionPending = enablePolicy_.transitionPending();
    facts.overflowCleanupActive = callbackOverflow_.active();
    facts.physicalConnectionPending = physicalConnectionPending();
    facts.notifyPending = controlNotifyPending_ || audioNotifyPending_;
    facts.callbackMailboxEmpty = callbackEvents_.empty();
    facts.notifyMailboxEmpty = notifyStatusEvents_.empty();
    return facts;
  }

  BLEServer *server_ = nullptr;
  BLECharacteristic *info_ = nullptr;
  BLECharacteristic *command_ = nullptr;
  BLECharacteristic *event_ = nullptr;
  BLECharacteristic *audio_ = nullptr;
  BlePeerPolicy peerPolicy_;
  BleAppHandshakePolicy appHandshake_;
  BleSingleConnectionPolicy connectionPolicy_;
  BleServiceEnablePolicy enablePolicy_;
  VoiceSessionController controller_;
  static constexpr size_t kCallbackEventCapacity = 16;
  static constexpr size_t kNotifyStatusEventCapacity = 4;
  static constexpr uint32_t kControlNotifyTimeoutMs = 40;
  BleVoiceCallbackMailbox<kCallbackEventCapacity> callbackEvents_;
  // notify() invokes its first status callback on the Arduino owner task. Keep
  // those events separate so the NimBLE-host callback mailbox remains SPSC.
  BleVoiceCallbackMailbox<kNotifyStatusEventCapacity> notifyStatusEvents_;
  BleVoiceNotifyCallbackBinding notifyCallbackBinding_;
  BleVoicePhysicalDisconnectLatch physicalDisconnects_;
  BleCallbackOverflowPolicy callbackOverflow_;
  BleVoiceCallbackSecuritySnapshot callbackSecurity_;
  mutable portMUX_TYPE qualityMux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable portMUX_TYPE notifyMux_ = portMUX_INITIALIZER_UNLOCKED;
  BleVoiceQualityCounters quality_;
  Print *log_ = nullptr;
  String deviceId_;
  bool connected_ = false;
  bool authenticated_ = false;
  bool appReady_ = false;
  bool bonded_ = false;
  uint16_t connectionId_ = 0;
  uint32_t connectionGeneration_ = 0;
  uint32_t sessionGeneration_ = 0;
  uint32_t notifyAttemptToken_ = 0;
  uint8_t currentPeerAddress_[6] = {};
  bool currentPeerAddressValid_ = false;
  uint16_t mtu_ = 23;
  uint32_t passkey_ = 0;
  uint32_t pairingUntilMs_ = 0;
  bool controlNotifyPending_ = false;
  uint32_t controlNotifyStartedAtMs_ = 0;
  ControlNotifyPurpose controlNotifyPurpose_ = ControlNotifyPurpose::none;
  BleVoiceNotifyIdentity controlNotifyIdentity_;
  volatile bool audioNotifyResolved_ = false;
  volatile bool audioNotifyAccepted_ = false;
  volatile bool audioNotifyPending_ = false;
  BleNotifyInFlight audioNotify_;
  BleVoiceNotifyIdentity audioNotifyIdentity_;
  BleVoiceAudioFrame audioInFlightFrame_;
  VoiceSessionError reportedError_ = VoiceSessionError::none;
  int batteryPercent_ = -1;
  BleConnectionPowerMode connectionPowerMode_ =
      BleConnectionPowerMode::idle;
  bool idlePaused_ = false;
  BleSessionStopRequestLatch sessionStopRequest_;
  bool callbackOverflowRecoveryRestartClaimed_ = false;
  bool appHandshakeRecoveryRestartClaimed_ = false;
};

}  // namespace pokepod
