#include "ProvisioningCoordinator.h"

#include "DeviceConfig.h"
#include "ProvisioningDiagnostics.h"
#include "ProvisioningPortal.h"
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

bool ProvisioningCoordinator::request(uint32_t nowMs) {
  if (portal_ == nullptr || wifi_ == nullptr || config_ == nullptr ||
      diagnostics_ == nullptr || log_ == nullptr) {
    return false;
  }
  if (!startup_.request(nowMs)) return false;
  normalWifiResumed_ = false;
  if (!portal_->prepare(*config_, *diagnostics_, *log_)) {
    startup_.fail();
    resumeNormalWifi();
    return false;
  }
  log_->println(
      "{\"event\":\"provisioning_startup\",\"phase\":\"requested\"}");
  return true;
}

void ProvisioningCoordinator::poll(uint32_t nowMs) {
  if (portal_ == nullptr || wifi_ == nullptr) return;
  if (startup_.pending()) {
    const ProvisioningStartupAction action = startup_.update(nowMs);
    if (action == ProvisioningStartupAction::quiesceRadio) {
      wifi_->quiesceForProvisioning(*log_);
      log_->println(
          "{\"event\":\"provisioning_startup\",\"phase\":\"quiescing\"}");
    } else if (action == ProvisioningStartupAction::switchRadioMode) {
      const bool completed = portal_->switchToAccessPointMode();
      startup_.finishStep(action, completed, nowMs);
      if (!completed) resumeNormalWifi();
    } else if (action == ProvisioningStartupAction::startAccessPoint) {
      const bool completed = portal_->startAccessPoint();
      startup_.finishStep(action, completed, nowMs);
      if (!completed) resumeNormalWifi();
    } else if (action == ProvisioningStartupAction::startPortalServices) {
      const bool completed = portal_->startServices();
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

void ProvisioningCoordinator::resumeNormalWifi() {
  if (normalWifiResumed_) return;
  normalWifiResumed_ = true;
  if (wifi_ != nullptr) wifi_->configurationChanged();
}

}  // namespace pokepod
