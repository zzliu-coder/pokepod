#include "BleVoiceService.h"

#include <BLESecurity.h>
#include <string.h>

#include "FirmwareVersion.h"

namespace pokepod {
namespace {

uintptr_t currentTaskToken() {
  return reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
}

class ServerCallbacks final : public BLEServerCallbacks {
 public:
  explicit ServerCallbacks(BleVoiceService &owner) : owner_(owner) {}

#if defined(CONFIG_BLUEDROID_ENABLED)
  void onConnect(BLEServer *, esp_ble_gatts_cb_param_t *param) override {
    owner_.handleConnect(param == nullptr ? kInvalidBleConnectionId
                                          : param->connect.conn_id,
                         param == nullptr ? nullptr
                                          : param->connect.remote_bda,
                         false);
  }
  void onDisconnect(BLEServer *, esp_ble_gatts_cb_param_t *param) override {
    owner_.handleDisconnect(param == nullptr ? kInvalidBleConnectionId
                                              : param->disconnect.conn_id);
  }
  void onMtuChanged(BLEServer *, esp_ble_gatts_cb_param_t *param) override {
    if (param != nullptr) {
      owner_.handleMtu(param->mtu.conn_id, param->mtu.mtu);
    }
  }
#elif defined(CONFIG_NIMBLE_ENABLED)
  void onConnect(BLEServer *, ble_gap_conn_desc *desc) override {
    owner_.handleConnect(desc == nullptr ? kInvalidBleConnectionId
                                         : desc->conn_handle,
                         desc == nullptr ? nullptr
                                         : desc->peer_id_addr.val,
                         desc != nullptr && desc->sec_state.bonded);
  }
  void onDisconnect(BLEServer *, ble_gap_conn_desc *desc) override {
    owner_.handleDisconnect(desc == nullptr ? kInvalidBleConnectionId
                                             : desc->conn_handle);
  }
  void onMtuChanged(BLEServer *, ble_gap_conn_desc *desc,
                    uint16_t mtu) override {
    if (desc != nullptr) owner_.handleMtu(desc->conn_handle, mtu);
  }
#endif

 private:
  BleVoiceService &owner_;
};

class CommandCallbacks final : public BLECharacteristicCallbacks {
 public:
  explicit CommandCallbacks(BleVoiceService &owner) : owner_(owner) {}

#if defined(CONFIG_BLUEDROID_ENABLED)
  void onWrite(BLECharacteristic *characteristic,
               esp_ble_gatts_cb_param_t *param) override {
    owner_.handleCommand(param == nullptr ? kInvalidBleConnectionId
                                          : param->write.conn_id,
                         characteristic->getData(),
                         characteristic->getLength());
  }
#elif defined(CONFIG_NIMBLE_ENABLED)
  void onWrite(BLECharacteristic *characteristic,
               ble_gap_conn_desc *desc) override {
    owner_.handleCommand(desc == nullptr ? kInvalidBleConnectionId
                                         : desc->conn_handle,
                         characteristic->getData(),
                         characteristic->getLength());
  }
#endif

 private:
  BleVoiceService &owner_;
};

class AudioCallbacks final : public BLECharacteristicCallbacks {
 public:
  explicit AudioCallbacks(BleVoiceService &owner) : owner_(owner) {}
  void onStatus(BLECharacteristic *, Status status, uint32_t) override {
    owner_.handleNotifyStatus(status == Status::SUCCESS_NOTIFY,
                              currentTaskToken());
  }

 private:
  BleVoiceService &owner_;
};

class ControlCallbacks final : public BLECharacteristicCallbacks {
 public:
  explicit ControlCallbacks(BleVoiceService &owner) : owner_(owner) {}
  void onStatus(BLECharacteristic *, Status status, uint32_t) override {
    owner_.handleControlNotifyStatus(status == Status::SUCCESS_NOTIFY,
                                     currentTaskToken());
  }

 private:
  BleVoiceService &owner_;
};

class DeviceInfoCallbacks final : public BLECharacteristicCallbacks {
 public:
  explicit DeviceInfoCallbacks(BleVoiceService &owner) : owner_(owner) {}
  void onRead(BLECharacteristic *) override { owner_.handleDeviceInfoRead(); }

 private:
  BleVoiceService &owner_;
};

class SecurityCallbacks final : public BLESecurityCallbacks {
 public:
  explicit SecurityCallbacks(BleVoiceService &owner) : owner_(owner) {}
  uint32_t onPassKeyRequest() override { return owner_.callbackPasskey(); }
  void onPassKeyNotify(uint32_t passkey) override {
    owner_.handlePasskey(passkey);
  }
  bool onSecurityRequest() override {
    return owner_.callbackSecurityAllowed();
  }
  bool onConfirmPIN(uint32_t pin) override {
    owner_.handlePasskey(pin);
    return owner_.callbackSecurityAllowed();
  }
  bool onAuthorizationRequest(uint16_t connectionId, uint16_t, bool) override {
    return owner_.callbackAuthorizationAllowed(connectionId);
  }
#if defined(CONFIG_BLUEDROID_ENABLED)
  void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
    owner_.handleAuthentication(kInvalidBleConnectionId, result.success,
                                result.bd_addr);
  }
#elif defined(CONFIG_NIMBLE_ENABLED)
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    owner_.handleAuthentication(desc == nullptr ? kInvalidBleConnectionId
                                                 : desc->conn_handle,
                                desc != nullptr && desc->sec_state.encrypted &&
                                    desc->sec_state.authenticated,
                                desc == nullptr ? nullptr
                                                : desc->peer_id_addr.val);
  }
#endif

 private:
  BleVoiceService &owner_;
};

ServerCallbacks *serverCallbacks = nullptr;
CommandCallbacks *commandCallbacks = nullptr;
AudioCallbacks *audioCallbacks = nullptr;
ControlCallbacks *controlCallbacks = nullptr;
DeviceInfoCallbacks *deviceInfoCallbacks = nullptr;
SecurityCallbacks *securityCallbacks = nullptr;

void forgetAllBonds() {
#if defined(CONFIG_BLUEDROID_ENABLED)
  int count = esp_ble_get_bond_device_num();
  if (count <= 0) return;
  esp_ble_bond_dev_t *devices = static_cast<esp_ble_bond_dev_t *>(
      malloc(sizeof(esp_ble_bond_dev_t) * count));
  if (devices == nullptr) return;
  if (esp_ble_get_bond_device_list(&count, devices) == ESP_OK) {
    for (int index = 0; index < count; ++index) {
      esp_ble_remove_bond_device(devices[index].bd_addr);
    }
  }
  free(devices);
#elif defined(CONFIG_NIMBLE_ENABLED)
  ble_store_clear();
#endif
}

bool hasBond() {
#if defined(CONFIG_BLUEDROID_ENABLED)
  return esp_ble_get_bond_device_num() > 0;
#elif defined(CONFIG_NIMBLE_ENABLED)
  int count = 0;
  return ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count) == 0 &&
      count > 0;
#else
  return false;
#endif
}

bool isBondedPeer(const uint8_t *peerAddress) {
  if (peerAddress == nullptr) return false;
#if defined(CONFIG_BLUEDROID_ENABLED)
  int count = esp_ble_get_bond_device_num();
  if (count <= 0) return false;
  esp_ble_bond_dev_t *devices = static_cast<esp_ble_bond_dev_t *>(
      malloc(sizeof(esp_ble_bond_dev_t) * count));
  if (devices == nullptr) return false;
  bool found = false;
  if (esp_ble_get_bond_device_list(&count, devices) == ESP_OK) {
    for (int index = 0; index < count; ++index) {
      if (memcmp(devices[index].bd_addr, peerAddress, 6) == 0) {
        found = true;
        break;
      }
    }
  }
  free(devices);
  return found;
#elif defined(CONFIG_NIMBLE_ENABLED)
  ble_addr_t peers[8];
  int count = 0;
  if (ble_store_util_bonded_peers(peers, &count, 8) != 0) return false;
  for (int index = 0; index < count; ++index) {
    if (memcmp(peers[index].val, peerAddress, 6) == 0) return true;
  }
  return false;
#else
  return false;
#endif
}

void retainOnlyBond(const uint8_t *peerAddress) {
  if (peerAddress == nullptr) return;
#if defined(CONFIG_BLUEDROID_ENABLED)
  int count = esp_ble_get_bond_device_num();
  if (count <= 1) return;
  esp_ble_bond_dev_t *devices = static_cast<esp_ble_bond_dev_t *>(
      malloc(sizeof(esp_ble_bond_dev_t) * count));
  if (devices == nullptr) return;
  if (esp_ble_get_bond_device_list(&count, devices) == ESP_OK) {
    for (int index = 0; index < count; ++index) {
      if (memcmp(devices[index].bd_addr, peerAddress, 6) != 0) {
        esp_ble_remove_bond_device(devices[index].bd_addr);
      }
    }
  }
  free(devices);
#elif defined(CONFIG_NIMBLE_ENABLED)
  ble_addr_t peers[8];
  int count = 0;
  if (ble_store_util_bonded_peers(peers, &count, 8) == 0) {
    for (int index = 0; index < count; ++index) {
      if (memcmp(peers[index].val, peerAddress, 6) != 0) {
        ble_store_util_delete_peer(&peers[index]);
      }
    }
  }
#endif
}

}  // namespace

bool BleVoiceService::begin(const String &deviceId, bool userEnabled,
                            Print &log) {
  log_ = &log;
  deviceId_ = deviceId;
  enablePolicy_.begin(userEnabled);
  idlePaused_ = false;
  const String advertisedName = "PokePod-" + deviceId.substring(
      deviceId.length() > 4 ? deviceId.length() - 4 : 0);
  BLEDevice::init(advertisedName.c_str());
  if (BLEDevice::setMTU(517) != ESP_OK) {
    log.println("{\"event\":\"ble_voice\",\"stage\":\"preferred_mtu\",\"ok\":false}");
  }
  BLESecurity::setAuthenticationMode(true, true, true);
  BLESecurity::setCapability(ESP_IO_CAP_OUT);
  BLESecurity::setKeySize(16);
  passkey_ = BLESecurity::generateRandomPassKey();
  BLESecurity::setPassKey(true, passkey_);
  BLESecurity::regenPassKeyOnConnect(true);
  bonded_ = hasBond();
  refreshCallbackSnapshot(millis());

  server_ = BLEDevice::createServer();
  if (server_ == nullptr) return false;
#if !defined(CONFIG_BT_NIMBLE_EXT_ADV) || defined(CONFIG_BLUEDROID_ENABLED)
  server_->advertiseOnDisconnect(false);
#endif
  serverCallbacks = new ServerCallbacks(*this);
  commandCallbacks = new CommandCallbacks(*this);
  audioCallbacks = new AudioCallbacks(*this);
  controlCallbacks = new ControlCallbacks(*this);
  deviceInfoCallbacks = new DeviceInfoCallbacks(*this);
  securityCallbacks = new SecurityCallbacks(*this);
  if (serverCallbacks == nullptr || commandCallbacks == nullptr ||
      audioCallbacks == nullptr || controlCallbacks == nullptr ||
      deviceInfoCallbacks == nullptr ||
      securityCallbacks == nullptr) return false;
  server_->setCallbacks(serverCallbacks);
  BLEDevice::setSecurityCallbacks(securityCallbacks);

  BLEService *service = server_->createService(kBleVoiceServiceUuid);
  if (service == nullptr) return false;
  info_ = service->createCharacteristic(
      kBleVoiceDeviceInfoUuid,
      BLECharacteristic::PROPERTY_READ |
          BLECharacteristic::PROPERTY_READ_ENC |
          BLECharacteristic::PROPERTY_READ_AUTHEN);
  command_ = service->createCharacteristic(
      kBleVoiceCommandUuid,
      BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_ENC |
          BLECharacteristic::PROPERTY_WRITE_AUTHEN);
  event_ = service->createCharacteristic(
      kBleVoiceEventUuid, BLECharacteristic::PROPERTY_NOTIFY);
  audio_ = service->createCharacteristic(
      kBleVoiceAudioUuid, BLECharacteristic::PROPERTY_NOTIFY);
  if (info_ == nullptr || command_ == nullptr || event_ == nullptr ||
      audio_ == nullptr) return false;
#if defined(CONFIG_BLUEDROID_ENABLED)
  info_->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
  command_->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
#endif
  command_->setCallbacks(commandCallbacks);
  event_->setCallbacks(controlCallbacks);
  audio_->setCallbacks(audioCallbacks);
  info_->setCallbacks(deviceInfoCallbacks);
  updateDeviceInfo();
  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(kBleVoiceServiceUuid);
  advertising->setScanResponse(true);
  // 0.625 ms units: advertise every 200-400 ms while disconnected. Bonded
  // Macs still reconnect quickly without keeping the radio in a dense burst.
  advertising->setMinInterval(320);
  advertising->setMaxInterval(640);
  if (userEnabled) advertising->start();
  log.printf("{\"event\":\"ble_voice\",\"ok\":true,\"enabled\":%s}\n",
             userEnabled ? "true" : "false");
  return true;
}

void BleVoiceService::requestEnable() {
  applyEnableActions(enablePolicy_.requestEnable(connected_), millis());
}

void BleVoiceService::requestDisable(uint32_t nowMs) {
  cancelPairingMode();
  sessionStopRequest_.beginTransition(enablePolicy_.transitionPending());
  applyEnableActions(
      enablePolicy_.requestDisable(controller_.active(), connected_, nowMs),
      nowMs);
}

void BleVoiceService::applyEnableActions(
    const BleServiceEnableActions &actions, uint32_t nowMs) {
  if (actions.stopAdvertising) {
    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    if (advertising != nullptr) advertising->stop();
  }
  if (actions.requestSessionStop) sessionStopRequest_.request();
  if (actions.disconnect && connected_ && server_ != nullptr) {
    server_->disconnect(connectionPolicy_.currentConnectionId());
  }
  if (actions.clearRuntime) clearDisabledRuntime(nowMs);
  if (actions.startAdvertising) restartAdvertising();
}

void BleVoiceService::clearDisabledRuntime(uint32_t nowMs) {
  pairingUntilMs_ = 0;
  connectionPolicy_.cancelPairingRequest();
  controller_.complete();
  clearControlNotify();
  resetAudioNotify();
  connected_ = authenticated_ = appReady_ = false;
  peerPolicy_.disconnected();
  mtu_ = 23;
  connectionId_ = 0;
  connectionGeneration_ = 0;
  connectionPowerMode_ = BleConnectionPowerMode::idle;
  currentPeerAddressValid_ = false;
  memset(currentPeerAddress_, 0, sizeof(currentPeerAddress_));
  callbackSecurity_.clearConnection();
  refreshCallbackSnapshot(nowMs);
}

void BleVoiceService::setBatteryPercent(int batteryPercent) {
  if (batteryPercent < -1) batteryPercent = -1;
  if (batteryPercent > 100) batteryPercent = 100;
  if (batteryPercent_ == batteryPercent) return;
  batteryPercent_ = batteryPercent;
  updateDeviceInfo();
}

void BleVoiceService::updateDeviceInfo() {
  if (info_ == nullptr) return;
  const BleVoiceQualitySnapshot current = quality();
  const String info = String("{\"batteryPercent\":") + batteryPercent_ +
      ",\"codec\":\"ima-adpcm\",\"deviceId\":\"" + deviceId_ +
      "\",\"firmwareVersion\":\"" + kFirmwareVersion +
      "\",\"lastErrorCode\":" +
      current.lastErrorCode + ",\"notifyAccepted\":" +
      current.notifyAccepted + ",\"notifyAttempts\":" +
      current.notifyAttempts + ",\"notifyFailures\":" +
      current.notifyFailures + ",\"notifyRetries\":" +
      current.notifyRetries + ",\"notifyAborts\":" +
      current.notifyAborts + ",\"disconnectedSessions\":" +
      current.disconnectedSessions + ",\"controlNotifyAttempts\":" +
      current.controlNotifyAttempts + ",\"controlNotifyAccepted\":" +
      current.controlNotifyAccepted + ",\"controlNotifyFailures\":" +
      current.controlNotifyFailures + ",\"protocolVersion\":1," +
      "\"queueOverflows\":" + current.queueOverflows +
      ",\"readyTimeouts\":" + current.readyTimeouts +
      ",\"sampleRateHz\":16000,\"sessionFailures\":" +
      current.sessionFailures + ",\"stopAckTimeouts\":" +
      current.stopAckTimeouts + ",\"streamTimeouts\":" +
      current.streamTimeouts + "}";
  info_->setValue(info);
}

BleVoiceQualitySnapshot BleVoiceService::quality() const {
  portENTER_CRITICAL(&qualityMux_);
  const BleVoiceQualitySnapshot value = quality_.snapshot();
  portEXIT_CRITICAL(&qualityMux_);
  return value;
}

void BleVoiceService::handleNotifyStatus(bool acceptedByHost,
                                         uintptr_t callbackTask) {
  BleVoiceCallbackEvent event;
  event.type = BleVoiceCallbackEventType::audioNotifyStatus;
  if (!notifyCallbackBinding_.capture(BleVoiceNotifyKind::audio,
                                      callbackTask,
                                      event.notifyIdentity)) {
    return;
  }
  event.connectionId = event.notifyIdentity.connectionId;
  event.connectionGeneration =
      event.notifyIdentity.connectionGeneration;
  event.flag = acceptedByHost;
  event.occurredAtMs = millis();
  notifyStatusEvents_.publish(event);
}

void BleVoiceService::handleControlNotifyStatus(bool acceptedByHost,
                                                uintptr_t callbackTask) {
  BleVoiceCallbackEvent event;
  event.type = BleVoiceCallbackEventType::controlNotifyStatus;
  if (!notifyCallbackBinding_.capture(BleVoiceNotifyKind::control,
                                      callbackTask,
                                      event.notifyIdentity)) {
    return;
  }
  event.connectionId = event.notifyIdentity.connectionId;
  event.connectionGeneration =
      event.notifyIdentity.connectionGeneration;
  event.flag = acceptedByHost;
  event.occurredAtMs = millis();
  notifyStatusEvents_.publish(event);
}

void BleVoiceService::handleDeviceInfoRead() {
  BleVoiceCallbackEvent event;
  event.type = BleVoiceCallbackEventType::deviceInfoRead;
  event.occurredAtMs = millis();
  publishCallbackEvent(event);
}

bool BleVoiceService::publishCallbackEvent(
    const BleVoiceCallbackEvent &event) {
  return callbackEvents_.publish(event);
}

void BleVoiceService::drainCallbackEvents(uint32_t nowMs) {
  if (callbackEvents_.overflowed() || notifyStatusEvents_.overflowed()) {
    callbackEvents_.closeAdmission();
    notifyStatusEvents_.closeAdmission();
    if (!callbackOverflowHandled_) {
      callbackOverflowHandled_ = true;
      if (connectionPolicy_.hasCurrent()) {
        callbackOverflowEpoch_.connectionId =
            connectionPolicy_.currentConnectionId();
        callbackOverflowEpoch_.generation = connectionGeneration_;
      } else {
        callbackOverflowEpoch_.connectionId = callbackSecurity_.connectionId();
        callbackOverflowEpoch_.generation =
            callbackSecurity_.connectionGeneration();
      }
      failClosedCallbackOverflow(nowMs, callbackOverflowEpoch_);
    }
    finishCallbackOverflowIfDisconnected(nowMs);
    return;
  }

  BleVoiceCallbackEvent event;
  size_t consumed = 0;
  while (consumed < kCallbackEventCapacity && callbackEvents_.take(event)) {
    processCallbackEvent(event, nowMs);
    ++consumed;
    if (callbackEvents_.overflowed()) break;
  }
  if (callbackEvents_.overflowed()) {
    drainCallbackEvents(nowMs);
    return;
  }
  drainNotifyStatusEvents(nowMs);
  if (notifyStatusEvents_.overflowed()) drainCallbackEvents(nowMs);
}

void BleVoiceService::drainNotifyStatusEvents(uint32_t nowMs) {
  BleVoiceCallbackEvent event;
  size_t consumed = 0;
  while (consumed < kNotifyStatusEventCapacity &&
         notifyStatusEvents_.take(event)) {
    processCallbackEvent(event, nowMs);
    ++consumed;
    if (notifyStatusEvents_.overflowed()) break;
  }
}

void BleVoiceService::processCallbackEvent(
    const BleVoiceCallbackEvent &event, uint32_t nowMs) {
  if (!enablePolicy_.acceptsNewWork() &&
      !bleCallbackAllowedDuringDisable(
          event, connectionPolicy_.hasCurrent(),
          connectionPolicy_.hasCurrent()
              ? connectionPolicy_.currentConnectionId()
              : kInvalidBleConnectionId,
          connectionGeneration_)) {
    return;
  }
  const uint32_t eventAtMs = event.occurredAtMs == 0
      ? nowMs
      : event.occurredAtMs;
  switch (event.type) {
    case BleVoiceCallbackEventType::connect:
      processConnect(event.connectionId, event.connectionGeneration,
                     event.peerAddressValid ? event.peerAddress : nullptr,
                     event.flag);
      break;
    case BleVoiceCallbackEventType::disconnect:
      processDisconnect(event.connectionId,
                        event.connectionGeneration, eventAtMs);
      break;
    case BleVoiceCallbackEventType::mtu:
      processMtu(event.connectionId,
                 event.connectionGeneration, event.value16);
      break;
    case BleVoiceCallbackEventType::command:
      processCommand(event.connectionId, event.data, event.dataLength,
                     event.connectionGeneration, eventAtMs);
      break;
    case BleVoiceCallbackEventType::authentication:
      processAuthentication(
          event.connectionId, event.flag,
          event.peerAddressValid ? event.peerAddress : nullptr,
          event.connectionGeneration, eventAtMs);
      break;
    case BleVoiceCallbackEventType::audioNotifyStatus:
      processNotifyStatus(event);
      break;
    case BleVoiceCallbackEventType::controlNotifyStatus:
      processControlNotifyStatus(event, eventAtMs);
      break;
    case BleVoiceCallbackEventType::deviceInfoRead:
      processDeviceInfoRead();
      break;
    case BleVoiceCallbackEventType::passkey:
      processPasskey(event.value32);
      break;
  }
}

void BleVoiceService::processNotifyStatus(
    const BleVoiceCallbackEvent &event) {
  if (!audioNotifyPending_ ||
      !event.notifyIdentity.matches(audioNotifyIdentity_) ||
      event.notifyIdentity.connectionGeneration != connectionGeneration_ ||
      event.notifyIdentity.sessionGeneration != sessionGeneration_) {
    return;
  }
  portENTER_CRITICAL(&qualityMux_);
  quality_.recordNotifyStatus(event.flag);
  portEXIT_CRITICAL(&qualityMux_);
  audioNotifyAccepted_ = event.flag;
  audioNotifyResolved_ = true;
}

void BleVoiceService::processControlNotifyStatus(
    const BleVoiceCallbackEvent &event, uint32_t nowMs) {
  if (!controlNotifyPending_ ||
      !event.notifyIdentity.matches(controlNotifyIdentity_) ||
      event.notifyIdentity.connectionGeneration != connectionGeneration_ ||
      event.notifyIdentity.sessionGeneration != sessionGeneration_) {
    return;
  }
  portENTER_CRITICAL(&qualityMux_);
  quality_.recordControlNotifyStatus(event.flag);
  portEXIT_CRITICAL(&qualityMux_);
  const ControlNotifyPurpose purpose = controlNotifyPurpose_;
  clearControlNotify();
  if (event.flag && purpose == ControlNotifyPurpose::sessionEnd) {
    controller_.markSessionEndSent(nowMs);
    return;
  }
  if (!event.flag &&
      (purpose == ControlNotifyPurpose::sessionStart ||
       purpose == ControlNotifyPurpose::sessionEnd)) {
    failAudioNotify(VoiceSessionError::notifyFailed);
    return;
  }
}

void BleVoiceService::processDeviceInfoRead() {
  updateDeviceInfo();
}

void BleVoiceService::poll(uint32_t nowMs) {
  drainCallbackEvents(nowMs);
  applyEnableActions(enablePolicy_.poll(controller_.active(), connected_,
                                        nowMs), nowMs);
  if (pairingUntilMs_ != 0 && !pairingMode(nowMs)) pairingUntilMs_ = 0;
  refreshCallbackSnapshot(nowMs);
  if (controlNotifyPending_ &&
      nowMs - controlNotifyStartedAtMs_ >= kControlNotifyTimeoutMs) {
    clearControlNotify();
    portENTER_CRITICAL(&qualityMux_);
    quality_.recordControlNotifyStatus(false);
    portEXIT_CRITICAL(&qualityMux_);
    if (connectionPolicy_.hasCurrent()) {
      const uint16_t connectionId = connectionPolicy_.currentConnectionId();
      if (server_ != nullptr) server_->disconnect(connectionId);
      processDisconnect(connectionId, connectionGeneration_, nowMs);
    }
  }
  if (!controller_.poll(nowMs) &&
      controller_.error() != reportedError_) {
    reportedError_ = controller_.error();
    resetAudioNotify();
    portENTER_CRITICAL(&qualityMux_);
    quality_.recordSessionError(reportedError_);
    portEXIT_CRITICAL(&qualityMux_);
    updateDeviceInfo();
    notifyControl(BleVoiceEventType::error, controller_.sessionId(),
                  static_cast<uint16_t>(reportedError_));
  }
  const VoiceSessionState state = controller_.state();
  if (connected_ && connectionPowerMode_ == BleConnectionPowerMode::voice &&
      !controller_.active()) {
    requestConnectionPowerMode(BleConnectionPowerMode::idle);
  }
  if (!connected_ || !authenticated_ || audio_ == nullptr ||
      (state != VoiceSessionState::streaming &&
       state != VoiceSessionState::ending)) return;
  bool resolved = false;
  bool accepted = false;
  bool pending = false;
  portENTER_CRITICAL(&notifyMux_);
  resolved = audioNotifyResolved_;
  accepted = audioNotifyAccepted_;
  pending = audioNotifyPending_;
  if (resolved) {
    audioNotifyResolved_ = false;
    audioNotifyPending_ = false;
  }
  portEXIT_CRITICAL(&notifyMux_);

  if (resolved) audioNotify_.resolve(accepted, nowMs);
  audioNotify_.poll(nowMs);
  if (pending && !resolved &&
      audioNotify_.snapshot().state != BleNotifyInFlightState::awaitingHost) {
    portENTER_CRITICAL(&notifyMux_);
    audioNotifyPending_ = false;
    portEXIT_CRITICAL(&notifyMux_);
    audioNotifyIdentity_ = {};
    notifyCallbackBinding_.invalidate();
  }

  if (audioNotify_.accepted()) {
    if (!controller_.commitFrame(audioNotify_.sequence())) {
      failAudioNotify(VoiceSessionError::notifyFailed);
      return;
    }
    resetAudioNotify();
  } else if (audioNotify_.failed()) {
    failAudioNotify(VoiceSessionError::notifyFailed);
    return;
  }

  if (audioNotify_.snapshot().state == BleNotifyInFlightState::idle &&
      controller_.peekFrame(audioInFlightFrame_)) {
    audioNotify_.start(readVoiceU32(audioInFlightFrame_.bytes + 6), nowMs);
  }

  if (audioNotify_.canAttempt(nowMs) && audioNotify_.beginAttempt(nowMs)) {
    if (audioNotify_.attempts() > 1) {
      portENTER_CRITICAL(&qualityMux_);
      quality_.recordNotifyRetry();
      portEXIT_CRITICAL(&qualityMux_);
    }
    audio_->setValue(audioInFlightFrame_.bytes,
                     sizeof(audioInFlightFrame_.bytes));
    portENTER_CRITICAL(&notifyMux_);
    audioNotifyPending_ = true;
    audioNotifyResolved_ = false;
    audioNotifyAccepted_ = false;
    portEXIT_CRITICAL(&notifyMux_);
    audioNotifyIdentity_ = nextNotifyIdentity(BleVoiceNotifyKind::audio);
    if (!beginNotifyCallback(audioNotifyIdentity_)) {
      failAudioNotify(VoiceSessionError::notifyFailed);
      return;
    }
    portENTER_CRITICAL(&qualityMux_);
    quality_.recordNotifyAttempt();
    portEXIT_CRITICAL(&qualityMux_);
    audio_->notify();
    endNotifyCallback();
  }
  if (controller_.state() == VoiceSessionState::ending &&
      controller_.queuedFrames() == 0 &&
      audioNotify_.snapshot().state == BleNotifyInFlightState::idle &&
      !controlNotifyPending_) {
    const uint32_t sessionId = controller_.sessionId();
    notifyControl(BleVoiceEventType::sessionEnd, sessionId);
  }
}

void BleVoiceService::enterPairingMode(uint32_t nowMs) {
  if (!enablePolicy_.acceptsNewWork()) return;
  if (connectionPolicy_.hasCurrent()) {
    pairingUntilMs_ = 0;
    connectionPolicy_.requestPairingAfterDisconnect();
    if (server_ != nullptr) {
      server_->disconnect(connectionPolicy_.currentConnectionId());
    }
    return;
  }
  activatePairingMode(nowMs);
}

void BleVoiceService::activatePairingMode(uint32_t nowMs) {
  if (!enablePolicy_.acceptsNewWork()) return;
  pairingUntilMs_ = nowMs + 120000;
  passkey_ = BLESecurity::generateRandomPassKey();
  BLESecurity::setPassKey(true, passkey_);
  refreshCallbackSnapshot(nowMs);
  restartAdvertising();
}

void BleVoiceService::cancelPairingMode() {
  pairingUntilMs_ = 0;
  connectionPolicy_.cancelPairingRequest();
  refreshCallbackSnapshot(millis());
  if (!bonded_ && connected_ && server_ != nullptr) {
    server_->disconnect(connectionPolicy_.currentConnectionId());
  }
}

void BleVoiceService::forgetMac() {
  controller_.complete();
  resetAudioNotify();
  forgetAllBonds();
  bonded_ = false;
  peerPolicy_.forgotBonds();
  appReady_ = authenticated_ = false;
  pairingUntilMs_ = 0;
  connectionPolicy_.cancelPairingRequest();
  refreshCallbackSnapshot(millis());
  if (server_ != nullptr && connected_) {
    server_->disconnect(connectionPolicy_.currentConnectionId());
  }
}

bool BleVoiceService::pauseForIdleSleep() {
  if (controller_.active() || pairingUntilMs_ != 0 || disablePending()) {
    return false;
  }
  idlePaused_ = true;
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  if (advertising != nullptr) advertising->stop();
  if (connected_ && server_ != nullptr) {
    server_->disconnect(connectionPolicy_.currentConnectionId());
    return false;
  }
  return true;
}

void BleVoiceService::resumeAfterIdleSleep() {
  if (!idlePaused_) return;
  idlePaused_ = false;
  restartAdvertising();
}

void BleVoiceService::prepareForDeepSleep() {
  idlePaused_ = true;
  controller_.complete();
  resetAudioNotify();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  if (advertising != nullptr) advertising->stop();
  if (connected_ && server_ != nullptr) {
    server_->disconnect(connectionPolicy_.currentConnectionId());
  }
  BLEDevice::deinit(false);
  connected_ = authenticated_ = appReady_ = false;
  callbackSecurity_.clearConnection();
  refreshCallbackSnapshot(millis());
}

bool BleVoiceService::startSession(uint32_t sessionId, uint32_t nowMs,
                                   AudioCaptureRouter &router) {
  if (!enablePolicy_.acceptsNewWork() || !appReady() || !authenticated_ ||
      !controller_.begin(sessionId, nowMs, connected_, mtu_, router)) {
    return false;
  }
  ++sessionGeneration_;
  if (sessionGeneration_ == 0) ++sessionGeneration_;
  requestConnectionPowerMode(BleConnectionPowerMode::voice);
  resetAudioNotify();
  reportedError_ = VoiceSessionError::none;
  if (notifyControl(BleVoiceEventType::sessionStart, sessionId, mtu_)) {
    return true;
  }
  controller_.complete();
  resetAudioNotify();
  return false;
}

void BleVoiceService::endSession() {
  if (!controller_.active()) return;
  controller_.end();
}

bool BleVoiceService::appendAudio(const uint8_t *stereo48, size_t bytes,
                                  uint32_t nowMs) {
  return controller_.appendStereo48(stereo48, bytes, nowMs);
}

bool BleVoiceService::appendMono16(const int16_t *samples, size_t count,
                                   uint32_t nowMs) {
  // poll() records a terminal controller error exactly once.  Counting the
  // overflow here as well would duplicate the same failed session.
  return controller_.appendMono16(samples, count, nowMs);
}

void BleVoiceService::abortSession(VoiceSessionError error) {
  controller_.abort(error);
}

void BleVoiceService::handleConnect(uint16_t connectionId,
                                    const uint8_t *peerAddress,
                                    bool peerBonded) {
  BleVoiceCallbackEvent event;
  event.type = BleVoiceCallbackEventType::connect;
  event.connectionId = connectionId;
  event.occurredAtMs = millis();
  event.flag = peerBonded;
  event.peerAddressValid = peerAddress != nullptr;
  if (peerAddress != nullptr) {
    memcpy(event.peerAddress, peerAddress, sizeof(event.peerAddress));
  }
  callbackSecurity_.observeConnect(connectionId, peerBonded,
                                   &event.connectionGeneration);
  publishCallbackEvent(event);
}

void BleVoiceService::processConnect(uint16_t connectionId,
                                     uint32_t connectionGeneration,
                                     const uint8_t *peerAddress,
                                     bool peerBonded) {
  if (!enablePolicy_.acceptsNewWork()) {
    if (server_ != nullptr) server_->disconnect(connectionId);
    return;
  }
  const BleConnectDecision decision = connectionPolicy_.connect(connectionId);
  if (decision == BleConnectDecision::rejectSecondary) {
    if (log_ != nullptr) {
      log_->println("{\"event\":\"ble_voice_secondary_rejected\"}");
    }
    if (server_ != nullptr) server_->disconnect(connectionId);
    return;
  }
  if (decision == BleConnectDecision::alreadyCurrent) return;
  if (connectionGeneration == 0) {
    connectionPolicy_.disconnect(connectionId);
    if (server_ != nullptr) server_->disconnect(connectionId);
    return;
  }
  connected_ = true;
  authenticated_ = false;
  appReady_ = false;
  connectionId_ = connectionId;
  connectionGeneration_ = connectionGeneration;
  connectionPowerMode_ = BleConnectionPowerMode::voice;
  requestConnectionPowerMode(BleConnectionPowerMode::idle);
  currentPeerAddressValid_ = peerAddress != nullptr;
  if (currentPeerAddressValid_) {
    memcpy(currentPeerAddress_, peerAddress, sizeof(currentPeerAddress_));
  } else {
    memset(currentPeerAddress_, 0, sizeof(currentPeerAddress_));
  }
  peerPolicy_.connected(peerBonded || isBondedPeer(peerAddress));
  mtu_ = 23;
  refreshCallbackSnapshot(millis());
  if (log_ != nullptr) log_->println("{\"event\":\"ble_voice_connected\"}");
}

void BleVoiceService::handleDisconnect(uint16_t connectionId) {
  BleVoiceCallbackEvent event;
  event.type = BleVoiceCallbackEventType::disconnect;
  event.connectionId = connectionId;
  event.occurredAtMs = millis();
  if (!callbackSecurity_.observeDisconnect(connectionId,
                                           &event.connectionGeneration)) {
    return;
  }
  physicalDisconnects_.observe(connectionId, event.connectionGeneration);
  publishCallbackEvent(event);
}

void BleVoiceService::processDisconnect(uint16_t connectionId,
                                        uint32_t connectionGeneration,
                                        uint32_t nowMs) {
  if (connectionGeneration == 0 ||
      connectionGeneration != connectionGeneration_) {
    if (log_ != nullptr) {
      log_->println("{\"event\":\"ble_voice_stale_disconnect_ignored\"}");
    }
    return;
  }
  if (!connectionPolicy_.disconnect(connectionId)) {
    if (log_ != nullptr) {
      log_->println("{\"event\":\"ble_voice_stale_disconnect_ignored\"}");
    }
    if (!connected_ && !idlePaused_) restartAdvertising();
    return;
  }
  const bool startPairing = connectionPolicy_.consumePairingAfterDisconnect();
  if (controller_.active()) {
    controller_.abort(VoiceSessionError::disconnected);
    reportedError_ = VoiceSessionError::disconnected;
    portENTER_CRITICAL(&qualityMux_);
    quality_.recordSessionError(reportedError_);
    portEXIT_CRITICAL(&qualityMux_);
  } else {
    controller_.complete();
    reportedError_ = VoiceSessionError::none;
  }
  resetAudioNotify();
  clearControlNotify();
  connected_ = authenticated_ = appReady_ = false;
  peerPolicy_.disconnected();
  mtu_ = 23;
  connectionId_ = 0;
  connectionGeneration_ = 0;
  connectionPowerMode_ = BleConnectionPowerMode::idle;
  currentPeerAddressValid_ = false;
  memset(currentPeerAddress_, 0, sizeof(currentPeerAddress_));
  callbackSecurity_.clearConnection();
  refreshCallbackSnapshot(nowMs);
  if (log_ != nullptr) log_->println("{\"event\":\"ble_voice_disconnected\"}");
  if (startPairing && enablePolicy_.acceptsNewWork()) {
    activatePairingMode(nowMs);
  } else if (!idlePaused_) {
    restartAdvertising();
  }
}

void BleVoiceService::requestConnectionPowerMode(
    BleConnectionPowerMode mode) {
  if (!connected_ || server_ == nullptr || connectionPowerMode_ == mode) return;
  const BleConnectionParameters parameters = bleConnectionParameters(mode);
  if (server_->requestConnParams(connectionId_, parameters.minInterval,
                                 parameters.maxInterval,
                                 parameters.latency,
                                 parameters.timeout)) {
    connectionPowerMode_ = mode;
    if (log_ != nullptr) {
      log_->printf("{\"event\":\"ble_connection_power\",\"mode\":\"%s\"}\n",
                   mode == BleConnectionPowerMode::voice ? "voice" : "idle");
    }
  }
}

void BleVoiceService::handleMtu(uint16_t connectionId, uint16_t mtu) {
  BleVoiceCallbackEvent event;
  event.type = BleVoiceCallbackEventType::mtu;
  event.connectionId = connectionId;
  event.connectionGeneration = callbackSecurity_.generationFor(connectionId);
  event.value16 = mtu;
  event.occurredAtMs = millis();
  publishCallbackEvent(event);
}

void BleVoiceService::processMtu(uint16_t connectionId,
                                 uint32_t connectionGeneration,
                                 uint16_t mtu) {
  if (!connectionPolicy_.isCurrent(connectionId) ||
      connectionGeneration != connectionGeneration_) return;
  mtu_ = mtu;
  if (log_ != nullptr) {
    log_->printf("{\"event\":\"ble_voice_mtu\",\"mtu\":%u,\"ready\":%s}\n",
                 mtu_, mtuReady() ? "true" : "false");
  }
}

void BleVoiceService::handleCommand(uint16_t connectionId,
                                    const uint8_t *bytes, size_t length) {
  BleVoiceCallbackEvent event;
  event.type = BleVoiceCallbackEventType::command;
  event.connectionId = connectionId;
  event.connectionGeneration = callbackSecurity_.generationFor(connectionId);
  event.occurredAtMs = millis();
  event.dataLength = length > UINT8_MAX ? UINT8_MAX
                                        : static_cast<uint8_t>(length);
  const size_t copyLength =
      length < sizeof(event.data) ? length : sizeof(event.data);
  if (bytes != nullptr && copyLength != 0) {
    memcpy(event.data, bytes, copyLength);
  }
  publishCallbackEvent(event);
}

void BleVoiceService::processCommand(uint16_t connectionId,
                                     const uint8_t *bytes, size_t length,
                                     uint32_t connectionGeneration,
                                     uint32_t nowMs) {
  if (!connectionPolicy_.commandAllowed(connectionId) ||
      connectionGeneration != connectionGeneration_) {
    if (log_ != nullptr) {
      log_->println("{\"event\":\"ble_voice_stale_command_ignored\"}");
    }
    return;
  }
  if (!peerPolicy_.commandAllowed(connected_, authenticated_,
                                  pairingMode(nowMs))) {
    if (log_ != nullptr) {
      log_->println("{\"event\":\"ble_voice_command_rejected\",\"reason\":\"unauthorized_peer\"}");
    }
    if (connected_ && server_ != nullptr) server_->disconnect(connectionId);
    return;
  }
  BleVoiceControl command;
  if (!decodeBleVoiceControl(bytes, length, command)) {
    notifyControl(BleVoiceEventType::error, 0, 1);
    return;
  }
  const auto type = static_cast<BleVoiceCommandType>(command.type);
  if (type == BleVoiceCommandType::ready) {
    if (command.sessionId == 0) {
      appReady_ = authenticated_ && mtuReady();
      notifyControl(BleVoiceEventType::status, 0, appReady_ ? 1 : 2);
    } else {
      controller_.markReady(command.sessionId, nowMs);
    }
  } else if (type == BleVoiceCommandType::reject && command.code == 100) {
    forgetMac();
  } else if (type == BleVoiceCommandType::reject) {
    if (bleVoiceCommandTargetsSession(command.sessionId,
                                      controller_.sessionId())) {
      controller_.complete();
      resetAudioNotify();
    }
  } else if (type == BleVoiceCommandType::stopAck) {
    if (controller_.acceptsStopAck(command.sessionId)) {
      controller_.complete();
      resetAudioNotify();
    }
  } else if (type == BleVoiceCommandType::ping) {
    notifyControl(BleVoiceEventType::status, command.sessionId,
                  appReady_ ? 1 : 2);
  }
  refreshCallbackSnapshot(nowMs);
}

uint16_t BleVoiceService::connectionIdForPeer(
    const uint8_t *peerAddress) const {
  if (!connectionPolicy_.hasCurrent() || !currentPeerAddressValid_ ||
      peerAddress == nullptr ||
      memcmp(currentPeerAddress_, peerAddress,
             sizeof(currentPeerAddress_)) != 0) {
    return kInvalidBleConnectionId;
  }
  return connectionPolicy_.currentConnectionId();
}

void BleVoiceService::handleAuthentication(uint16_t connectionId, bool success,
                                           const uint8_t *peerAddress) {
  BleVoiceCallbackEvent event;
  event.type = BleVoiceCallbackEventType::authentication;
  event.connectionId = connectionId == kInvalidBleConnectionId
      ? callbackSecurity_.connectionId()
      : connectionId;
  event.connectionGeneration =
      callbackSecurity_.generationFor(event.connectionId);
  event.flag = success;
  event.occurredAtMs = millis();
  event.peerAddressValid = peerAddress != nullptr;
  if (peerAddress != nullptr) {
    memcpy(event.peerAddress, peerAddress, sizeof(event.peerAddress));
  }
  publishCallbackEvent(event);
}

void BleVoiceService::processAuthentication(uint16_t connectionId,
                                            bool success,
                                            const uint8_t *peerAddress,
                                            uint32_t connectionGeneration,
                                            uint32_t nowMs) {
  if (connectionId == kInvalidBleConnectionId) {
    connectionId = connectionIdForPeer(peerAddress);
  }
  if (!connectionPolicy_.authenticationAllowed(connectionId) ||
      connectionGeneration != connectionGeneration_ ||
      connectionIdForPeer(peerAddress) != connectionId) {
    if (log_ != nullptr) {
      log_->println("{\"event\":\"ble_voice_stale_auth_ignored\"}");
    }
    return;
  }
  const bool allowed = success && securityAllowed(nowMs);
  authenticated_ = allowed;
  if (allowed) {
    retainOnlyBond(peerAddress);
    bonded_ = true;
    peerPolicy_.authenticatedAndBonded();
    pairingUntilMs_ = 0;
  }
  if (!allowed && server_ != nullptr) server_->disconnect(connectionId);
  refreshCallbackSnapshot(nowMs);
  if (log_ != nullptr) {
    log_->printf("{\"event\":\"ble_voice_auth\",\"ok\":%s}\n",
                 allowed ? "true" : "false");
  }
}

void BleVoiceService::handlePasskey(uint32_t passkey) {
  BleVoiceCallbackEvent event;
  event.type = BleVoiceCallbackEventType::passkey;
  event.value32 = passkey;
  event.occurredAtMs = millis();
  publishCallbackEvent(event);
}

void BleVoiceService::processPasskey(uint32_t passkey) {
  passkey_ = passkey;
  refreshCallbackSnapshot(millis());
  if (log_ != nullptr) {
    log_->println("{\"event\":\"ble_voice_passkey_updated\"}");
  }
}

bool BleVoiceService::notifyControl(BleVoiceEventType type, uint32_t sessionId,
                                    uint16_t code) {
  if (!connected_ || event_ == nullptr || controlNotifyPending_) return false;
  BleVoiceControl control;
  control.type = static_cast<uint8_t>(type);
  control.sessionId = sessionId;
  control.code = code;
  uint8_t bytes[8];
  const size_t size = encodeBleVoiceControl(control, bytes, sizeof(bytes));
  if (size == 0) return false;
  event_->setValue(bytes, size);
  controlNotifyIdentity_ = nextNotifyIdentity(BleVoiceNotifyKind::control);
  if (!beginNotifyCallback(controlNotifyIdentity_)) {
    controlNotifyIdentity_ = {};
    return false;
  }
  controlNotifyPending_ = true;
  controlNotifyStartedAtMs_ = millis();
  if (type == BleVoiceEventType::sessionStart) {
    controlNotifyPurpose_ = ControlNotifyPurpose::sessionStart;
  } else if (type == BleVoiceEventType::sessionEnd) {
    controlNotifyPurpose_ = ControlNotifyPurpose::sessionEnd;
  } else if (type == BleVoiceEventType::error) {
    controlNotifyPurpose_ = ControlNotifyPurpose::error;
  } else {
    controlNotifyPurpose_ = ControlNotifyPurpose::generic;
  }
  portENTER_CRITICAL(&qualityMux_);
  quality_.recordControlNotifyAttempt();
  portEXIT_CRITICAL(&qualityMux_);
  event_->notify();
  endNotifyCallback();
  // NimBLE may report host-queue acceptance synchronously from notify(). The
  // callback only enqueues that result; poll() resolves this pending control.
  return true;
}

void BleVoiceService::clearControlNotify() {
  notifyCallbackBinding_.invalidate();
  controlNotifyPending_ = false;
  controlNotifyStartedAtMs_ = 0;
  controlNotifyPurpose_ = ControlNotifyPurpose::none;
  controlNotifyIdentity_ = {};
}

void BleVoiceService::resetAudioNotify() {
  notifyCallbackBinding_.invalidate();
  audioNotify_.reset();
  audioNotifyIdentity_ = {};
  memset(&audioInFlightFrame_, 0, sizeof(audioInFlightFrame_));
  portENTER_CRITICAL(&notifyMux_);
  audioNotifyPending_ = false;
  audioNotifyResolved_ = false;
  audioNotifyAccepted_ = false;
  portEXIT_CRITICAL(&notifyMux_);
}

BleVoiceNotifyIdentity BleVoiceService::nextNotifyIdentity(
    BleVoiceNotifyKind kind) {
  BleVoiceNotifyIdentity identity;
  if (!connectionPolicy_.hasCurrent() || connectionGeneration_ == 0) {
    return identity;
  }
  ++notifyAttemptToken_;
  if (notifyAttemptToken_ == 0) ++notifyAttemptToken_;
  identity.kind = kind;
  identity.connectionId = connectionPolicy_.currentConnectionId();
  identity.connectionGeneration = connectionGeneration_;
  identity.sessionGeneration = sessionGeneration_;
  identity.attemptToken = notifyAttemptToken_;
  return identity;
}

bool BleVoiceService::beginNotifyCallback(
    const BleVoiceNotifyIdentity &identity) {
  return notifyCallbackBinding_.begin(identity, currentTaskToken());
}

void BleVoiceService::endNotifyCallback() {
  notifyCallbackBinding_.invalidate();
}

void BleVoiceService::failAudioNotify(VoiceSessionError error) {
  clearControlNotify();
  resetAudioNotify();
  controller_.abort(error);
  reportedError_ = error;
  portENTER_CRITICAL(&qualityMux_);
  quality_.recordSessionError(error);
  portEXIT_CRITICAL(&qualityMux_);
  updateDeviceInfo();
  notifyControl(BleVoiceEventType::error, controller_.sessionId(),
                static_cast<uint16_t>(error));
}

void BleVoiceService::failClosedCallbackOverflow(
    uint32_t nowMs, const BleVoiceConnectionEpoch &epoch) {
  if (log_ != nullptr) {
    log_->println(
        "{\"event\":\"ble_voice_callback_overflow\",\"action\":\"disconnect\"}");
  }
  notifyCallbackBinding_.invalidate();
  if (controller_.active()) {
    controller_.abort(VoiceSessionError::disconnected);
    reportedError_ = VoiceSessionError::disconnected;
    portENTER_CRITICAL(&qualityMux_);
    quality_.recordSessionError(reportedError_);
    portEXIT_CRITICAL(&qualityMux_);
  } else {
    controller_.complete();
    reportedError_ = VoiceSessionError::none;
  }
  resetAudioNotify();
  clearControlNotify();
  connected_ = authenticated_ = appReady_ = false;
  peerPolicy_.disconnected();
  mtu_ = 23;
  connectionPowerMode_ = BleConnectionPowerMode::idle;
  currentPeerAddressValid_ = false;
  memset(currentPeerAddress_, 0, sizeof(currentPeerAddress_));
  refreshCallbackSnapshot(nowMs);
  if (epoch.valid() && server_ != nullptr) {
    server_->disconnect(epoch.connectionId);
  }
}

bool BleVoiceService::finishCallbackOverflowIfDisconnected(uint32_t nowMs) {
  if (!callbackOverflowHandled_) return false;
  bool confirmed = !callbackOverflowEpoch_.valid();
  BleVoiceConnectionEpoch physicalDisconnect;
  if (!confirmed && physicalDisconnects_.latest(physicalDisconnect)) {
    confirmed = physicalDisconnect.matches(callbackOverflowEpoch_);
  }
  if (!confirmed) return false;

  const BleVoiceConnectionEpoch closedEpoch = callbackOverflowEpoch_;
  callbackEvents_.resetAfterOverflow();
  notifyStatusEvents_.resetAfterOverflow();
  callbackOverflowHandled_ = false;
  callbackOverflowEpoch_ = {};

  if (closedEpoch.valid() && connectionPolicy_.hasCurrent() &&
      connectionPolicy_.currentConnectionId() == closedEpoch.connectionId &&
      connectionGeneration_ == closedEpoch.generation) {
    processDisconnect(closedEpoch.connectionId, closedEpoch.generation,
                      nowMs);
  } else if (!idlePaused_) {
    restartAdvertising();
  }
  return true;
}

void BleVoiceService::refreshCallbackSnapshot(uint32_t nowMs) {
  const bool pairingAllowed = pairingMode(nowMs);
  callbackSecurity_.refreshFromMain(
      connectionPolicy_.hasCurrent(),
      connectionPolicy_.hasCurrent()
          ? connectionPolicy_.currentConnectionId()
          : kInvalidBleConnectionId,
      peerPolicy_.securityAllowed(pairingAllowed), pairingAllowed, passkey_);
}

void BleVoiceService::restartAdvertising() {
  if (!enablePolicy_.acceptsNewWork() || idlePaused_ ||
      connectionPolicy_.hasCurrent()) return;
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  if (advertising != nullptr) advertising->start();
}

}  // namespace pokepod
