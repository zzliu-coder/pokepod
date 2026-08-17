#pragma once

#include <Arduino.h>

namespace pokepod {

class AudioPipeline;
class BleVoiceService;
class BoardServices;
class CapabilityRegistry;
class CapsuleLibrary;
class Dashboard;
class DeviceConfig;
class PowerDiagnostics;
class ProvisioningCoordinator;
class ProvisioningDiagnostics;
class RuntimePowerManager;
class RuntimeDiagnostics;
class TencentWorker;
class UsbLinkBridge;
class WavRecorder;
class WifiController;

// Builds the frozen Link v2 diagnostic payloads from live subsystem facts.
// It owns no transport/request lifecycle and never sends frames by itself.
class LinkDiagnostics {
 public:
  void bind(BoardServices &board,
            AudioPipeline &audio,
            UsbLinkBridge &usb,
            BleVoiceService &bleVoice,
            Dashboard &dashboard,
            CapsuleLibrary &library,
            WavRecorder &recorder,
            DeviceConfig &config,
            WifiController &wifi,
            TencentWorker &tencent,
            ProvisioningDiagnostics &provisioningDiagnostics,
            PowerDiagnostics &powerDiagnostics,
            RuntimePowerManager &power,
            ProvisioningCoordinator *provisioningCoordinator,
            const CapabilityRegistry *capabilities,
            RuntimeDiagnostics *runtimeDiagnostics = nullptr);

  // The returned reference remains valid until the next statusJson() call.
  // Its backing allocation is reserved once during bind so repeated status
  // requests cannot fragment the small internal heap.
  const String &statusJson() const;
  String provisioningJson() const;
  String powerJson() const;
  String runtimeJson() const;
  String runtimeTraceJson(size_t newestOffset, size_t limit) const;
  bool clearProvisioning(Print &log) const;
  bool clearPower(Print &log) const;
  bool clearRuntime(Print &log) const;

 private:
  BoardServices *board_ = nullptr;
  AudioPipeline *audio_ = nullptr;
  UsbLinkBridge *usb_ = nullptr;
  BleVoiceService *bleVoice_ = nullptr;
  Dashboard *dashboard_ = nullptr;
  CapsuleLibrary *library_ = nullptr;
  WavRecorder *recorder_ = nullptr;
  DeviceConfig *config_ = nullptr;
  WifiController *wifi_ = nullptr;
  TencentWorker *tencent_ = nullptr;
  ProvisioningDiagnostics *provisioningDiagnostics_ = nullptr;
  PowerDiagnostics *powerDiagnostics_ = nullptr;
  RuntimePowerManager *power_ = nullptr;
  ProvisioningCoordinator *provisioningCoordinator_ = nullptr;
  const CapabilityRegistry *capabilities_ = nullptr;
  RuntimeDiagnostics *runtimeDiagnostics_ = nullptr;
  mutable String statusBuffer_;
};

}  // namespace pokepod
