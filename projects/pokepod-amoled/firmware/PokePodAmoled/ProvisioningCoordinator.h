#pragma once

#include <Arduino.h>

#include "ProvisioningStartupPolicy.h"
#include "RuntimeDiagnosticsCodec.h"

namespace pokepod {

class DeviceConfig;
class BleVoiceService;
class ProvisioningDiagnostics;
class ProvisioningPortal;
class RuntimeDiagnostics;
class WifiController;
enum class ProvisioningStopReason : uint8_t;

class ProvisioningCoordinator {
 public:
  bool begin(ProvisioningPortal &portal, WifiController &wifi,
             DeviceConfig &config, ProvisioningDiagnostics &diagnostics,
             Print &log);
  void bindRuntimeDiagnostics(RuntimeDiagnostics &diagnostics) {
    runtimeDiagnostics_ = &diagnostics;
  }
  void bindBleVoice(BleVoiceService &bleVoice) { bleVoice_ = &bleVoice; }
  bool request(uint32_t nowMs);
  void poll(uint32_t nowMs);
  void stop(ProvisioningStopReason reason);

  bool visible() const { return startup_.visible(); }
  bool active() const;
  bool pending() const { return startup_.pending(); }
  bool ownsWifi() const { return startup_.ownsWifi(); }
  bool sensitiveConfirmationPending() const;
  bool confirmSensitiveChange(uint32_t nowMs);
  ProvisioningStartupPhase phase() const { return startup_.phase(); }
  const char *phaseName() const {
    return provisioningStartupPhaseName(startup_.phase());
  }
  bool takeConfigurationChanged();
  bool takeRestartRequired();

 private:
  void resumeNormalWifi();
  void recordRuntime(RuntimeDiagnosticStage stage,
                     RuntimeDiagnosticOutcome outcome,
                     uint32_t detail0, uint32_t detail1);

  ProvisioningPortal *portal_ = nullptr;
  WifiController *wifi_ = nullptr;
  DeviceConfig *config_ = nullptr;
  ProvisioningDiagnostics *diagnostics_ = nullptr;
  Print *log_ = nullptr;
  RuntimeDiagnostics *runtimeDiagnostics_ = nullptr;
  BleVoiceService *bleVoice_ = nullptr;
  ProvisioningStartupPolicy startup_;
  bool normalWifiResumed_ = true;
  bool blePauseRequested_ = false;
  bool restartRequired_ = false;
};

}  // namespace pokepod
