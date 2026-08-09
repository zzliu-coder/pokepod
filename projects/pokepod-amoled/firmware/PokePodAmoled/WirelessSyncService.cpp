#include "WirelessSyncService.h"

#include "AudioCaptureRouter.h"
#include "AudioPipeline.h"
#include "BleVoiceService.h"
#include "BoardServices.h"
#include "CapsuleLibrary.h"
#include "Dashboard.h"
#include "DeviceConfig.h"
#include "ProvisioningDiagnostics.h"
#include "RuntimePowerManager.h"
#include "TencentWorker.h"
#include "UsbLinkBridge.h"
#include "WavRecorder.h"
#include "WifiController.h"
#include "WirelessSyncIdentity.h"

namespace pokepod {
namespace {
constexpr uint32_t kAuthenticationTimeoutMs = 6000;
}

bool WirelessSyncService::begin(
    fs::FS &fs, BoardServices &board, AudioPipeline &audio,
    AudioCaptureRouter &captureRouter, UsbLinkBridge &usb,
    BleVoiceService &bleVoice, Dashboard &dashboard,
    CapsuleLibrary &library, WavRecorder &recorder, DeviceConfig &config,
    WifiController &wifi, TencentWorker &tencent,
    ProvisioningDiagnostics &provisioningDiagnostics,
    RuntimePowerManager &power, WirelessSyncIdentity &identity,
    LinkServiceCoordinator &coordinator, Print &log) {
  identity_ = &identity;
  log_ = &log;
  authenticator_.begin(identity, replay_, log);
  begun_ = link_.begin(tls_, fs, board, audio, captureRouter, usb, bleVoice,
                       dashboard, library, recorder, config, wifi, tencent,
                       provisioningDiagnostics, power, log, &coordinator,
                       LinkTransport::wifi, nullptr,
                       &window_.transferGate(), nullptr);
  log.printf("{\"event\":\"wifi_sync_service\",\"ok\":%s,\"tls_identity\":%s}\n",
             begun_ ? "true" : "false", identity.ready() ? "true" : "false");
  return begun_;
}

void WirelessSyncService::open(uint32_t nowMs) {
  if (!begun_) return;
  window_.open(nowMs);
  lastError_ = "";
  if (log_ != nullptr) {
    log_->println("{\"event\":\"wifi_sync_window\",\"open\":true,\"seconds\":300}");
  }
}

void WirelessSyncService::close() {
  window_.close();
  stopListener();
  closeClient("window-closed");
  decision_ = WirelessSyncWindowDecision();
  if (log_ != nullptr) {
    log_->println("{\"event\":\"wifi_sync_window\",\"open\":false}");
  }
}

bool WirelessSyncService::pairingBundle(bool rotate, String &json,
                                        String &error) {
  enforceDeadline(millis());
  if (!window_.opened()) {
    error = "open wireless sync on PokePod before USB pairing";
    return false;
  }
  if (identity_ == nullptr) {
    error = "wireless identity is unavailable";
    return false;
  }
  return identity_->pairingBundle(rotate, json, error);
}

bool WirelessSyncService::secureReady() const {
  return begun_ && identity_ != nullptr && identity_->ready();
}

uint32_t WirelessSyncService::remainingSeconds() const {
  return decision_.remainingMs == 0 ? 0 :
      (decision_.remainingMs + 999) / 1000;
}

void WirelessSyncService::enforceDeadline(uint32_t nowMs) {
  if (window_.deadlineReached(nowMs)) close();
}

void WirelessSyncService::poll(uint32_t nowMs, bool networkConnected) {
  if (!begun_) return;
  enforceDeadline(nowMs);
  if (!window_.opened()) return;
  WirelessSyncWindowInputs inputs;
  inputs.networkConnected = networkConnected;
  inputs.secureServerReady = secureReady();
  inputs.clientConnected = clientPresent_;
  decision_ = window_.update(nowMs, inputs);

  if (!decision_.listener) {
    stopListener();
  } else if (!listenerActive_ && !startListener()) {
    lastError_ = "listener-start-failed";
  }
  if (decision_.bonjour && listenerActive_) {
    const bool paired = identity_ != nullptr && identity_->paired();
    if (!bonjour_.active() || bonjour_.pairedAdvertised() != paired) {
      if (!bonjour_.start(kWirelessSyncPort, paired, *log_)) {
        lastError_ = "bonjour-start-failed";
      }
    }
  } else {
    bonjour_.stop();
  }

  if (!window_.opened()) {
    closeClient("window-closed");
    return;
  }
  if (!networkConnected || !secureReady()) {
    closeClient(!networkConnected ? "network-unavailable" :
                                     "secure-server-unavailable");
    return;
  }
  if (!clientPresent_ && listenerActive_) acceptClient(nowMs);
  if (!clientPresent_) return;

  if (tls_.phase() == WirelessTlsPhase::handshaking) {
    tls_.pollHandshake(nowMs);
  }
  if (tls_.failed() || tls_.closed()) {
    closeClient(tls_.failed() ? "tls-failed" : "peer-closed");
    return;
  }
  if (!tls_.ready()) return;

  if (!authenticator_.authenticated()) {
    authenticator_.poll(tls_);
    if (authenticator_.rejected()) {
      closeClient("authentication-failed");
      return;
    }
    if (!authenticator_.authenticated() &&
        static_cast<uint32_t>(nowMs - clientStartedAtMs_) >=
            kAuthenticationTimeoutMs) {
      closeClient("authentication-timeout");
      return;
    }
  }
  if (authenticator_.authenticated()) {
    if (!authenticationObserved_) {
      authenticationObserved_ = true;
      if (log_ != nullptr) {
        log_->println("{\"event\":\"wifi_sync_session\",\"authenticated\":true}");
      }
    }
    link_.poll(nowMs);
    if (tls_.failed() || tls_.closed()) {
      closeClient("link-disconnected");
    }
  }
}

bool WirelessSyncService::startListener() {
  if (!secureReady()) return false;
  server_.begin(kWirelessSyncPort, 1);
  server_.setNoDelay(true);
  listenerActive_ = static_cast<bool>(server_);
  if (log_ != nullptr) {
    log_->printf("{\"event\":\"wifi_sync_listener\",\"active\":%s,\"port\":%u}\n",
                 listenerActive_ ? "true" : "false", kWirelessSyncPort);
  }
  return listenerActive_;
}

void WirelessSyncService::stopListener() {
  bonjour_.stop();
  if (listenerActive_) server_.end();
  listenerActive_ = false;
}

void WirelessSyncService::acceptClient(uint32_t nowMs) {
  NetworkClient incoming = server_.accept();
  if (!incoming) return;
  authenticator_.reset();
  link_.disconnect();
  if (!tls_.begin(incoming, *identity_, window_.transferGate(), nowMs,
                  *log_)) {
    incoming.stop();
    lastError_ = "tls-initialization-failed";
    return;
  }
  clientPresent_ = true;
  authenticationObserved_ = false;
  clientStartedAtMs_ = nowMs == 0 ? 1 : nowMs;
  lastError_ = "";
}

void WirelessSyncService::closeClient(const char *reason) {
  if (!clientPresent_ && tls_.phase() != WirelessTlsPhase::handshaking &&
      tls_.phase() != WirelessTlsPhase::ready) {
    return;
  }
  link_.disconnect();
  tls_.close();
  authenticator_.reset();
  clientPresent_ = false;
  authenticationObserved_ = false;
  clientStartedAtMs_ = 0;
  if (reason != nullptr) lastError_ = reason;
  if (log_ != nullptr && reason != nullptr) {
    log_->printf("{\"event\":\"wifi_sync_session\",\"closed\":true,\"reason\":\"%s\"}\n",
                 reason);
  }
}

}  // namespace pokepod
