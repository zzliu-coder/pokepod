#include "ProvisioningCoordinator.h"

#include "DeviceConfig.h"
#include "ProvisioningDiagnostics.h"
#include "ProvisioningPortal.h"
#include "RuntimeDiagnostics.h"
#include "WifiController.h"

namespace pokepod {

bool ProvisioningCoordinator::begin(
    ProvisioningPortal &portal, WifiController &wifi, DeviceConfig &config,
    ProvisioningDiagnostics &diagnostics, Print &log) {
  portal_ = &portal;
  wifi_ = &wifi;
  config_ = &config;
  diagnostics_ = &diagnostics;
  log_ = &log;
  startup_.reset();
  normalWifiResumed_ = true;
  return true;
}

void ProvisioningCoordinator::recordRuntime(
    RuntimeDiagnosticStage stage, RuntimeDiagnosticOutcome outcome,
    uint32_t detail0, uint32_t detail1) {
  if (runtimeDiagnostics_ == nullptr || log_ == nullptr) return;
  (void)runtimeDiagnostics_->record(RuntimeDiagnosticSubsystem::provisioning,
                                    stage, outcome, detail0, detail1, *log_);
}

bool ProvisioningCoordinator::request(uint32_t nowMs) {
  if (portal_ == nullptr || wifi_ == nullptr || config_ == nullptr ||
      diagnostics_ == nullptr || log_ == nullptr) {
    return false;
  }
  if (!startup_.request(nowMs)) return false;
  recordRuntime(RuntimeDiagnosticStage::provisioningRequest,
                RuntimeDiagnosticOutcome::started, nowMs, 0);
  normalWifiResumed_ = false;
  if (!portal_->prepare(*config_, *diagnostics_, *log_)) {
    recordRuntime(RuntimeDiagnosticStage::provisioningRequest,
                  RuntimeDiagnosticOutcome::failure, 1, 0);
    startup_.fail();
    resumeNormalWifi();
    return false;
  }
  recordRuntime(RuntimeDiagnosticStage::provisioningRequest,
                RuntimeDiagnosticOutcome::success, nowMs, 0);
  log_->println(
      "{\"event\":\"provisioning_startup\",\"phase\":\"requested\"}");
  return true;
}

void ProvisioningCoordinator::poll(uint32_t nowMs) {
  if (portal_ == nullptr || wifi_ == nullptr) return;
  if (startup_.pending()) {
    const ProvisioningStartupAction action = startup_.update(nowMs);
    if (action == ProvisioningStartupAction::quiesceRadio) {
      recordRuntime(RuntimeDiagnosticStage::provisioningQuiesceBefore,
                    RuntimeDiagnosticOutcome::started, nowMs, 0);
      wifi_->quiesceForProvisioning(*log_);
      recordRuntime(RuntimeDiagnosticStage::provisioningQuiesceAfter,
                    RuntimeDiagnosticOutcome::success, nowMs, 0);
      log_->println(
          "{\"event\":\"provisioning_startup\",\"phase\":\"quiescing\"}");
    } else if (action == ProvisioningStartupAction::switchRadioMode) {
      recordRuntime(RuntimeDiagnosticStage::provisioningModeBefore,
                    RuntimeDiagnosticOutcome::started, nowMs, 0);
      const bool completed = portal_->switchToAccessPointMode();
      recordRuntime(RuntimeDiagnosticStage::provisioningModeAfter,
                    completed ? RuntimeDiagnosticOutcome::success
                              : RuntimeDiagnosticOutcome::failure,
                    completed ? 0U : 1U, nowMs);
      startup_.finishStep(action, completed, nowMs);
      if (!completed) resumeNormalWifi();
    } else if (action == ProvisioningStartupAction::startAccessPoint) {
      recordRuntime(RuntimeDiagnosticStage::provisioningSoftApBefore,
                    RuntimeDiagnosticOutcome::started, nowMs, 0);
      const bool completed = portal_->startAccessPoint();
      recordRuntime(RuntimeDiagnosticStage::provisioningSoftApAfter,
                    completed ? RuntimeDiagnosticOutcome::success
                              : RuntimeDiagnosticOutcome::failure,
                    completed ? 0U : 1U, nowMs);
      startup_.finishStep(action, completed, nowMs);
      if (!completed) resumeNormalWifi();
    } else if (action == ProvisioningStartupAction::startPortalServices) {
      recordRuntime(RuntimeDiagnosticStage::provisioningServicesBefore,
                    RuntimeDiagnosticOutcome::started, nowMs, 0);
      const bool completed = portal_->startServices();
      recordRuntime(RuntimeDiagnosticStage::provisioningServicesAfter,
                    completed ? RuntimeDiagnosticOutcome::success
                              : RuntimeDiagnosticOutcome::failure,
                    completed ? 0U : 1U, nowMs);
      startup_.finishStep(action, completed, nowMs);
      if (!completed) resumeNormalWifi();
    } else if (action == ProvisioningStartupAction::failTimeout) {
      portal_->failStartupTimeout();
      resumeNormalWifi();
    }
  }
  if (startup_.active()) {
    // Link v2 can create the request after the main loop captured nowMs.
    // The portal's lifetime starts during this poll, so give it a timestamp
    // sampled after startup rather than the caller's stale loop timestamp.
    if (portal_->active()) portal_->loop(millis());
    if (!portal_->active()) {
      startup_.reset();
      resumeNormalWifi();
    }
  }
}

void ProvisioningCoordinator::stop() {
  if (portal_ != nullptr) portal_->stop();
  startup_.reset();
  resumeNormalWifi();
}

bool ProvisioningCoordinator::active() const {
  return portal_ != nullptr && portal_->active();
}

bool ProvisioningCoordinator::takeConfigurationChanged() {
  return portal_ != nullptr && portal_->takeConfigurationChanged();
}

bool ProvisioningCoordinator::sensitiveConfirmationPending() const {
  return portal_ != nullptr && portal_->sensitiveConfirmationPending();
}

bool ProvisioningCoordinator::confirmSensitiveChange(uint32_t nowMs) {
  return portal_ != nullptr && portal_->confirmSensitiveChange(nowMs);
}

void ProvisioningCoordinator::resumeNormalWifi() {
  if (normalWifiResumed_) return;
  normalWifiResumed_ = true;
  if (wifi_ != nullptr) wifi_->configurationChanged();
}

}  // namespace pokepod
