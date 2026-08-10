#include "BleVoiceService.h"

#include <BLESecurity.h>
#include <string.h>

namespace pokepod {
namespace {

class ServerCallbacks final : public BLEServerCallbacks {
 public:
  explicit ServerCallbacks(BleVoiceService &owner) : owner_(owner) {}

#if defined(CONFIG_BLUEDROID_ENABLED)
  void onConnect(BLEServer *, esp_ble_gatts_cb_param_t *param) override {
    owner_.handleConnect(param == nullptr ? kInvalidBleConnectionId
                                          : param->connect.conn_id,
                         param == nullptr ? nullptr
                                          : param->connect.remote_bda);
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
                                         : desc->peer_id_addr.val);
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
    owner_.handleNotifyStatus(status == Status::SUCCESS_NOTIFY);
  }

 private:
  BleVoiceService &owner_;
};

class ControlCallbacks final : public BLECharacteristicCallbacks {
 public:
  explicit ControlCallbacks(BleVoiceService &owner) : owner_(owner) {}
  void onStatus(BLECharacteristic *, Status status, uint32_t) override {
    owner_.handleControlNotifyStatus(status == Status::SUCCESS_NOTIFY);
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
  uint32_t onPassKeyRequest() override { return owner_.passkey(); }
  void onPassKeyNotify(uint32_t passkey) override {
    owner_.handlePasskey(passkey);
  }
  bool onSecurityRequest() override {
    return owner_.securityAllowed(millis());
  }
  bool onConfirmPIN(uint32_t pin) override {
    owner_.handlePasskey(pin);
    return owner_.securityAllowed(millis());
  }
  bool onAuthorizationRequest(uint16_t connectionId, uint16_t, bool) override {
    return owner_.authorizationAllowed(connectionId, millis());
  }
#if defined(CONFIG_BLUEDROID_ENABLED)
  void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
    owner_.handleAuthentication(owner_.connectionIdForPeer(result.bd_addr),
                                result.success, result.bd_addr);
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

bool BleVoiceService::begin(const String &deviceId, Print &log) {
  log_ = &log;
  deviceId_ = deviceId;
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
  advertising->start();
  log.println("{\"event\":\"ble_voice\",\"ok\":true}");
  return true;
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
      "\",\"firmwareVersion\":\"2.0.0\",\"lastErrorCode\":" +
      current.lastErrorCode + ",\"notifyAccepted\":" +
      current.notifyAccepted + ",\"notifyAttempts\":" +
      current.notifyAttempts + ",\"notifyFailures\":" +
      current.notifyFailures + ",\"protocolVersion\":1," +
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

void BleVoiceService::handleNotifyStatus(bool acceptedByHost) {
  portENTER_CRITICAL(&qualityMux_);
  quality_.recordNotifyStatus(acceptedByHost);
  portEXIT_CRITICAL(&qualityMux_);
}

void BleVoiceService::handleControlNotifyStatus(bool acceptedByHost) {
  handleNotifyStatus(acceptedByHost);
  controlNotifyAccepted_ = acceptedByHost;
  controlNotifyResolved_ = true;
}

void BleVoiceService::handleDeviceInfoRead() {
  updateDeviceInfo();
}

void BleVoiceService::poll(uint32_t nowMs) {
  if (pairingUntilMs_ != 0 && !pairingMode(nowMs)) pairingUntilMs_ = 0;
  if (!controller_.poll(nowMs) &&
      controller_.error() != reportedError_) {
    reportedError_ = controller_.error();
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
  BleVoiceAudioFrame frame;
  uint8_t budget = 4;
  while (budget-- > 0 && controller_.takeFrame(frame)) {
    audio_->setValue(frame.bytes, sizeof(frame.bytes));
    portENTER_CRITICAL(&qualityMux_);
    quality_.recordNotifyAttempt();
    portEXIT_CRITICAL(&qualityMux_);
    audio_->notify();
  }
  if (controller_.state() == VoiceSessionState::ending &&
      controller_.queuedFrames() == 0) {
    const uint32_t sessionId = controller_.sessionId();
    if (notifyControl(BleVoiceEventType::sessionEnd, sessionId)) {
      controller_.markSessionEndSent(nowMs);
    }
  }
}

void BleVoiceService::enterPairingMode(uint32_t nowMs) {
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
  pairingUntilMs_ = nowMs + 120000;
  passkey_ = BLESecurity::generateRandomPassKey();
  BLESecurity::setPassKey(true, passkey_);
  restartAdvertising();
}

void BleVoiceService::cancelPairingMode() {
  pairingUntilMs_ = 0;
  connectionPolicy_.cancelPairingRequest();
  if (!bonded_ && connected_ && server_ != nullptr) {
    server_->disconnect(connectionPolicy_.currentConnectionId());
  }
}

void BleVoiceService::forgetMac() {
  controller_.complete();
  forgetAllBonds();
  bonded_ = false;
  peerPolicy_.forgotBonds();
  appReady_ = authenticated_ = false;
  pairingUntilMs_ = 0;
  connectionPolicy_.cancelPairingRequest();
  if (server_ != nullptr && connected_) {
    server_->disconnect(connectionPolicy_.currentConnectionId());
  }
}

bool BleVoiceService::startSession(uint32_t sessionId, uint32_t nowMs,
                                   AudioCaptureRouter &router) {
  if (!appReady() || !authenticated_ ||
      !controller_.begin(sessionId, nowMs, connected_, mtu_, router)) {
    return false;
  }
  requestConnectionPowerMode(BleConnectionPowerMode::voice);
  reportedError_ = VoiceSessionError::none;
  if (notifyControl(BleVoiceEventType::sessionStart, sessionId, mtu_)) {
    return true;
  }
  controller_.complete();
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

void BleVoiceService::handleConnect(uint16_t connectionId,
                                    const uint8_t *peerAddress) {
  const BleConnectDecision decision = connectionPolicy_.connect(connectionId);
  if (decision == BleConnectDecision::rejectSecondary) {
    if (log_ != nullptr) {
      log_->println("{\"event\":\"ble_voice_secondary_rejected\"}");
    }
    if (server_ != nullptr) server_->disconnect(connectionId);
    return;
  }
  if (decision == BleConnectDecision::alreadyCurrent) return;
  connected_ = true;
  authenticated_ = false;
  appReady_ = false;
  connectionId_ = connectionId;
  connectionPowerMode_ = BleConnectionPowerMode::voice;
  requestConnectionPowerMode(BleConnectionPowerMode::idle);
  currentPeerAddressValid_ = peerAddress != nullptr;
  if (currentPeerAddressValid_) {
    memcpy(currentPeerAddress_, peerAddress, sizeof(currentPeerAddress_));
  } else {
    memset(currentPeerAddress_, 0, sizeof(currentPeerAddress_));
  }
  peerPolicy_.connected(isBondedPeer(peerAddress));
  mtu_ = 23;
  if (log_ != nullptr) log_->println("{\"event\":\"ble_voice_connected\"}");
}

void BleVoiceService::handleDisconnect(uint16_t connectionId) {
  if (!connectionPolicy_.disconnect(connectionId)) {
    if (log_ != nullptr) {
      log_->println("{\"event\":\"ble_voice_stale_disconnect_ignored\"}");
    }
    return;
  }
  const bool startPairing = connectionPolicy_.consumePairingAfterDisconnect();
  controller_.complete();
  reportedError_ = VoiceSessionError::none;
  connected_ = authenticated_ = appReady_ = false;
  peerPolicy_.disconnected();
  mtu_ = 23;
  connectionId_ = 0;
  connectionPowerMode_ = BleConnectionPowerMode::idle;
  currentPeerAddressValid_ = false;
  memset(currentPeerAddress_, 0, sizeof(currentPeerAddress_));
  if (log_ != nullptr) log_->println("{\"event\":\"ble_voice_disconnected\"}");
  if (startPairing) {
    activatePairingMode(millis());
  } else {
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
  if (!connectionPolicy_.isCurrent(connectionId)) return;
  mtu_ = mtu;
  if (log_ != nullptr) {
    log_->printf("{\"event\":\"ble_voice_mtu\",\"mtu\":%u,\"ready\":%s}\n",
                 mtu_, mtuReady() ? "true" : "false");
  }
}

void BleVoiceService::handleCommand(uint16_t connectionId,
                                    const uint8_t *bytes, size_t length) {
  if (!connectionPolicy_.commandAllowed(connectionId)) {
    if (log_ != nullptr) {
      log_->println("{\"event\":\"ble_voice_stale_command_ignored\"}");
    }
    return;
  }
  if (!peerPolicy_.commandAllowed(connected_, authenticated_,
                                  pairingMode(millis()))) {
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
      controller_.markReady(command.sessionId, millis());
    }
  } else if (type == BleVoiceCommandType::reject && command.code == 100) {
    forgetMac();
  } else if (type == BleVoiceCommandType::reject) {
    if (bleVoiceCommandTargetsSession(command.sessionId,
                                      controller_.sessionId())) {
      controller_.complete();
    }
  } else if (type == BleVoiceCommandType::stopAck) {
    if (controller_.acceptsStopAck(command.sessionId)) {
      controller_.complete();
    }
  } else if (type == BleVoiceCommandType::ping) {
    notifyControl(BleVoiceEventType::status, command.sessionId,
                  appReady_ ? 1 : 2);
  }
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
  if (!connectionPolicy_.authenticationAllowed(connectionId) ||
      connectionIdForPeer(peerAddress) != connectionId) {
    if (log_ != nullptr) {
      log_->println("{\"event\":\"ble_voice_stale_auth_ignored\"}");
    }
    return;
  }
  const bool allowed = success && securityAllowed(millis());
  authenticated_ = allowed;
  if (allowed) {
    retainOnlyBond(peerAddress);
    bonded_ = true;
    peerPolicy_.authenticatedAndBonded();
    pairingUntilMs_ = 0;
  }
  if (!allowed && server_ != nullptr) server_->disconnect(connectionId);
  if (log_ != nullptr) {
    log_->printf("{\"event\":\"ble_voice_auth\",\"ok\":%s}\n",
                 allowed ? "true" : "false");
  }
}

void BleVoiceService::handlePasskey(uint32_t passkey) {
  passkey_ = passkey;
  if (log_ != nullptr) {
    log_->println("{\"event\":\"ble_voice_passkey_updated\"}");
  }
}

bool BleVoiceService::notifyControl(BleVoiceEventType type, uint32_t sessionId,
                                    uint16_t code) {
  if (!connected_ || event_ == nullptr) return false;
  BleVoiceControl control;
  control.type = static_cast<uint8_t>(type);
  control.sessionId = sessionId;
  control.code = code;
  uint8_t bytes[8];
  const size_t size = encodeBleVoiceControl(control, bytes, sizeof(bytes));
  if (size == 0) return false;
  event_->setValue(bytes, size);
  controlNotifyResolved_ = false;
  controlNotifyAccepted_ = false;
  portENTER_CRITICAL(&qualityMux_);
  quality_.recordNotifyAttempt();
  portEXIT_CRITICAL(&qualityMux_);
  event_->notify();
  // Arduino-ESP32 reports the NimBLE host queue result synchronously through
  // onStatus. This is host acceptance only; it is not an air-delivery ACK.
  return controlNotifyResolved_ && controlNotifyAccepted_;
}

void BleVoiceService::restartAdvertising() {
  if (connectionPolicy_.hasCurrent()) return;
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  if (advertising != nullptr) advertising->start();
}

}  // namespace pokepod
