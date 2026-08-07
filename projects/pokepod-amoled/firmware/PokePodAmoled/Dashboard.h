#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <FS.h>

#include "BoardServices.h"
#include "CapsuleLibrary.h"
#include "ChineseRenderer.h"
#include "DeviceConfig.h"
#include "UiPolicy.h"
#include "WifiPolicy.h"

namespace pokepod {

struct DashboardView {
  const BoardStatus *board = nullptr;
  const CapsuleLibrary *library = nullptr;
  const DeviceSettings *settings = nullptr;
  bool audioReady = false;
  bool usbReady = false;
  bool hostConnected = false;
  bool dictationHolding = false;
  bool recording = false;
  bool transcribing = false;
  bool playing = false;
  bool provisioning = false;
  uint32_t recordingMs = 0;
  uint16_t audioPeak = 0;
  WifiPhase wifiPhase = WifiPhase::disabled;
  int32_t wifiRssi = 0;
  String portalSsid;
  String portalPassword;
  String message;
};

class Dashboard {
 public:
  void begin(Arduino_GFX *display, fs::FS *fs = nullptr);
  void draw(const DashboardView &view);
  UiAction actionAt(int16_t x, int16_t y, bool hostConnected) const;
  void swipeHorizontal(int16_t deltaX, bool locked);
  void swipeVertical(int16_t deltaY, const CapsuleLibrary &library);
  bool openCapsuleAt(int16_t y, const CapsuleLibrary &library);
  void back();
  void invalidate() { invalidated_ = true; }
  const UiState &state() const { return state_; }
  const CapsuleSummary *selected(const CapsuleLibrary &library) const;
  bool sdFontReady() const { return renderer_.sdFontReady(); }
  uint32_t fullRedrawCount() const { return fullRedrawCount_; }
  uint32_t bodyRedrawCount() const { return bodyRedrawCount_; }
  uint32_t partialRedrawCount() const { return partialRedrawCount_; }

 private:
  void drawBody(const DashboardView &view);
  void drawTopBar(const DashboardView &view);
  void drawHome(const DashboardView &view);
  void drawCapsules(const DashboardView &view);
  void drawCapsuleDetail(const DashboardView &view);
  void drawDevice(const DashboardView &view);
  void drawPageDots();
  void drawButton(int16_t x, int16_t y, int16_t width, int16_t height,
                  uint16_t color, const String &title, const String &subtitle = "");
  void drawRecordingDynamic(const DashboardView &view);
  void drawDynamicRegions(const DashboardView &view);
  String signature(const DashboardView &view) const;
  String topBarSignature(const DashboardView &view) const;

  Arduino_GFX *display_ = nullptr;
  ChineseRenderer renderer_;
  UiState state_;
  bool invalidated_ = true;
  String lastSignature_;
  String lastTopBarSignature_;
  String lastMessage_;
  RootPage lastMessagePage_ = RootPage::home;
  bool lastDictationHolding_ = false;
  uint32_t lastRecordingSecond_ = UINT32_MAX;
  uint32_t fullRedrawCount_ = 0;
  uint32_t bodyRedrawCount_ = 0;
  uint32_t partialRedrawCount_ = 0;
  uint8_t listOffset_ = 0;
  uint16_t detailLineOffset_ = 0;
};

}  // namespace pokepod
