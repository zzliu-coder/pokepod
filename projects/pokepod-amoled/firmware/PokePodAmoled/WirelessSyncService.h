#pragma once

#include <Arduino.h>
#include <FS.h>
#include <NetworkServer.h>

#include "PokePodLinkService.h"
#include "WirelessSyncAuthenticator.h"
#include "WirelessSyncBonjour.h"
#include "WirelessSyncPairing.h"
#include "WirelessSecurityPolicy.h"
#include "WirelessSyncTlsStream.h"
#include "WirelessSyncWindow.h"

namespace pokepod {

class AudioPipeline;
class AudioCaptureRouter;
class BleVoiceService;
class BoardServices;
class CapsuleLibrary;
class Dashboard;
class DeviceConfig;
class ProvisioningDiagnostics;
class PowerDiagnostics;
class RuntimePowerManager;
class TencentWorker;
class UsbLinkBridge;
class WavRecorder;
class WifiController;
class WirelessSyncIdentity;

class WirelessSyncService : public WirelessSyncPairingProvider {
 public:
  bool begin(fs::FS &fs, BoardServices &board, AudioPipeline &audio,
             AudioCaptureRouter &captureRouter, UsbLinkBridge &usb,
             BleVoiceService &bleVoice, Dashboard &dashboard,
             CapsuleLibrary &library, WavRecorder &recorder,
             DeviceConfig &config, WifiController &wifi,
             TencentWorker &tencent,
             ProvisioningDiagnostics &provisioningDiagnostics,
             PowerDiagnostics &powerDiagnostics,
             RuntimePowerManager &power, WirelessSyncIdentity &identity,
             LinkServiceCoordinator &coordinator, Print &log);
  void poll(uint32_t nowMs, bool networkConnected);
  void enforceDeadline(uint32_t nowMs);
  void open(uint32_t nowMs);
  void close();
  bool pairingBundle(bool rotate, String &json, String &error) override;

  bool openWindow() const { return window_.opened(); }
  bool wifiDemand() const { return window_.opened(); }
  bool clientConnected() const { return clientPresent_; }
  bool authenticated() const { return authenticator_.authenticated(); }
  bool linkBusy() const {
    return link_.receivingBinary() || link_.maintenanceActive();
  }
  bool receivingBinary() const { return link_.receivingBinary(); }
  bool bonjourActive() const { return bonjour_.active(); }
  bool listenerActive() const { return listenerActive_; }
  bool secureReady() const;
  bool paired() const;
  bool networkConnected() const { return networkConnected_; }
  bool completed() const {
    return window_.opened() && lastCompletedAtMs_ != 0 && lastError_.isEmpty();
  }
  uint32_t lastCompletedAtMs() const { return lastCompletedAtMs_; }
  uint32_t remainingSeconds(uint32_t nowMs) const;
  WirelessSyncWindowPhase phase() const { return decision_.phase; }
  const char *lastError() const { return lastError_.c_str(); }

 private:
  bool startListener();
  void stopListener();
  void acceptClient(uint32_t nowMs);
  void closeClient(const char *reason);

  NetworkServer server_{kWirelessSyncPort, 1};
  WirelessSyncTlsStream tls_;
  WirelessSyncAuthenticator authenticator_;
  WirelessSyncBonjour bonjour_;
  WirelessSyncWindow window_;
  WirelessSyncWindowDecision decision_;
  WirelessReplayGuard replay_;
  WirelessAuthDeadline authenticationDeadline_;
  PokePodLinkService link_;
  WirelessSyncIdentity *identity_ = nullptr;
  Print *log_ = nullptr;
  bool begun_ = false;
  bool listenerActive_ = false;
  bool clientPresent_ = false;
  bool authenticationObserved_ = false;
  bool networkConnected_ = false;
  uint32_t observedMaintenanceStartRevision_ = 0;
  uint32_t observedMaintenanceCompletionRevision_ = 0;
  uint32_t lastCompletedAtMs_ = 0;
  String lastError_;
};

}  // namespace pokepod
