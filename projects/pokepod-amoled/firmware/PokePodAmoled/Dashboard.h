#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <FS.h>

#include "BoardServices.h"
#include "CapsuleLibrary.h"
#include "ChineseRenderer.h"
#include "DeviceConfig.h"
#include "PeakWindow.h"
#include "ProvisioningPolicy.h"
#include "ProvisioningDiagnostics.h"
#include "ScrollPhysics.h"
#include "UiIcons.h"
#include "UiMotionPolicy.h"
#include "UiPolicy.h"
#include "UiRenderPolicy.h"
#include "UiTheme.h"
#include "WifiPolicy.h"

namespace pokepod {

struct DashboardView {
  const BoardStatus *board = nullptr;
  const CapsuleLibrary *library = nullptr;
  const DeviceSettings *settings = nullptr;
  bool audioReady = false;
  bool usbReady = false;
  bool usbConnected = false;
  bool bleVoiceConnected = false;
  bool bleVoiceReady = false;
  bool bleVoiceBonded = false;
  bool bleVoicePairing = false;
  uint32_t bleVoicePasskey = 0;
  uint16_t bleVoiceMtu = 23;
  uint32_t bleVoiceNotifyFailures = 0;
  uint32_t bleVoiceQueueOverflows = 0;
  uint32_t bleVoiceReadyTimeouts = 0;
  uint32_t bleVoiceStopAckTimeouts = 0;
  uint32_t bleVoiceStreamTimeouts = 0;
  bool wifiSyncOpen = false;
  bool wifiSyncSecureReady = false;
  bool wifiSyncListener = false;
  bool wifiSyncBonjour = false;
  bool wifiSyncClient = false;
  bool wifiSyncAuthenticated = false;
  uint32_t wifiSyncRemainingSeconds = 0;
  bool wirelessHolding = false;
  bool recording = false;
  bool transcribing = false;
  bool playing = false;
  bool provisioning = false;
  bool undoAvailable = false;
  uint32_t recordingMs = 0;
  uint16_t audioPeak = 0;
  uint16_t audioEnvelope[PeakWindow::kEnvelopeSamples] = {};
  WifiPhase wifiPhase = WifiPhase::disabled;
  int32_t wifiRssi = 0;
  String portalSsid;
  String portalPassword;
  String portalStatus;
  ProvisioningState portalState = ProvisioningState::ready;
  const ProvisioningDiagnostics *provisioningDiagnostics = nullptr;
  String message;
};

class Dashboard {
 public:
  void begin(Arduino_GFX *display, fs::FS *fs = nullptr);
  void draw(const DashboardView &view);
  UiAction actionAt(int16_t x, int16_t y, bool voiceReady) const;
  void swipeHorizontal(int16_t deltaX, bool locked, int16_t startX);
  bool beginVerticalScroll(int16_t y, uint32_t nowMs,
                           const CapsuleLibrary &library);
  bool updateVerticalScroll(int16_t y, uint32_t nowMs,
                            const CapsuleLibrary &library);
  void endVerticalScroll(uint32_t nowMs);
  bool advanceVerticalScroll(uint32_t nowMs,
                             const CapsuleLibrary &library);
  bool scrollActive() const;
  bool openCapsuleAt(int16_t y, const CapsuleLibrary &library);
  bool beginCapsuleSelectionAt(int16_t y, const CapsuleLibrary &library);
  bool toggleCapsuleSelectionAt(int16_t y, const CapsuleLibrary &library);
  void clearCapsuleSelection();
  bool capsuleSelectionMode() const { return browserState_.selectionMode(); }
  std::vector<String> selectedCapsuleIds(const CapsuleLibrary &library) const;
  void scopeChanged();
  void openScopePicker();
  void openDetailMore();
  void openProvisioningLog();
  void closeOverlays();
  void back();
  void navigate(RootPage page);
  void invalidate() {
    invalidated_ = true;
    scrollFramePending_ = false;
  }
  const UiState &state() const { return state_; }
  const CapsuleSummary *selected(const CapsuleLibrary &library) const;
  bool sdFontReady() const { return renderer_.sdFontReady(); }
  uint32_t fullRedrawCount() const { return fullRedrawCount_; }
  uint32_t bodyRedrawCount() const { return bodyRedrawCount_; }
  uint32_t partialRedrawCount() const { return partialRedrawCount_; }
  bool frameBufferReady() const { return frame_ != nullptr; }
  bool animationBufferReady() const { return frame_ != nullptr; }

 private:
  enum class SettingAccessory : uint8_t { value, toggle, chevron };

  void drawBody(const DashboardView &view);
  void drawTopBar(const DashboardView &view);
  void drawPageIndicator();
  void drawBackButton();
  void drawHome(const DashboardView &view);
  void drawCapsules(const DashboardView &view);
  void drawCapsuleDetail(const DashboardView &view);
  void drawScopePicker(const DashboardView &view);
  void drawDetailMore(const DashboardView &view);
  void drawDevice(const DashboardView &view);
  void drawProvisioning(const DashboardView &view);
  void drawProvisioningLog(const DashboardView &view);
  void drawCapsuleOrb(int16_t centerY, uint16_t accent,
                      uint16_t dimAccent, int16_t scale = 100);
  void drawHomeAction(int16_t top, int16_t bottom, bool wireless,
                      bool holding, bool enabled = true);
  void drawToast(const String &message);
  void drawCenteredText(const String &text, int16_t y, UiTextSize size,
                        uint16_t color, bool bold = false,
                        int16_t maxWidth = 336);
  void drawSettingRow(int16_t top, UiIcon icon, const String &title,
                      const String &detail, uint16_t detailColor,
                      SettingAccessory accessory, bool toggleEnabled = false);
  void drawDetailAction(int16_t left, UiIcon icon, const String &label,
                        bool emphasized, uint16_t accent);
  void drawRecordingDynamic(const DashboardView &view, bool presentPartial);
  void drawDynamicRegions(const DashboardView &view);
  void presentFrame();
  void presentScrollRegion();
  void presentRegion(int16_t x, int16_t y, int16_t width, int16_t height);
  String signature(const DashboardView &view, bool includeScroll) const;
  String topBarSignature(const DashboardView &view) const;
  void reconcileCapsules(const CapsuleLibrary *library);
  ScrollPhysics *activeScroll();
  const ScrollPhysics *activeScroll() const;
  void updateScrollBounds(const CapsuleLibrary &library);
  int32_t capsuleScrollMaximum(const CapsuleLibrary &library) const;
  int32_t provisioningScrollMaximum() const;

  Arduino_GFX *output_ = nullptr;
  Arduino_GFX *display_ = nullptr;
  Arduino_Canvas_Indexed *frame_ = nullptr;
  ChineseRenderer renderer_;
  UiState state_;
  bool invalidated_ = true;
  bool scrollFramePending_ = false;
  String lastSignature_;
  String lastStableSignature_;
  String lastTopBarSignature_;
  bool lastWirelessHolding_ = false;
  bool lastRecording_ = false;
  uint16_t smoothedPeak_ = 0;
  uint16_t envelopeCeiling_ = 1200;
  uint8_t animationTick_ = 0;
  uint32_t fullRedrawCount_ = 0;
  uint32_t bodyRedrawCount_ = 0;
  uint32_t partialRedrawCount_ = 0;
  CapsuleBrowserState browserState_;
  ScrollPhysics capsuleScroll_;
  ScrollPhysics detailScroll_;
  ScrollPhysics provisioningLogScroll_;
  String detailBodyCache_;
  String detailBodyCacheKey_;
  uint32_t lastLibraryRevision_ = 0xffffffffU;
  uint8_t provisioningLogCount_ = 0;
};

}  // namespace pokepod
