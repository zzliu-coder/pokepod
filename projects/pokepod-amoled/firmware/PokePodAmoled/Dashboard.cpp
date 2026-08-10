#include "Dashboard.h"

#include <new>

#include "TimePolicy.h"
#include "WifiUiPolicy.h"

namespace pokepod {
namespace {

String capsuleStatus(const CapsuleSummary &record) {
  if (record.readOnly) return "只读";
  if (record.status == CapsuleStatus::queued && !record.error.isEmpty()) {
    return "网络重试";
  }
  switch (record.status) {
    case CapsuleStatus::queued: return "待转写";
    case CapsuleStatus::transcribing: return "转写中";
    case CapsuleStatus::rawReady: return "转写完成";
    case CapsuleStatus::correcting: return "校对中";
    case CapsuleStatus::ready: return "完成";
    case CapsuleStatus::failed: return "需要重试";
    case CapsuleStatus::recording: return "录音中";
    case CapsuleStatus::damaged: return "需要检查";
  }
  return "";
}

uint16_t capsuleStatusColor(CapsuleStatus status) {
  switch (status) {
    case CapsuleStatus::failed:
    case CapsuleStatus::damaged: return ui::kError;
    case CapsuleStatus::queued:
    case CapsuleStatus::transcribing:
    case CapsuleStatus::correcting: return ui::kWaiting;
    case CapsuleStatus::recording:
    case CapsuleStatus::rawReady:
    case CapsuleStatus::ready: return ui::kAccent;
  }
  return ui::kMuted;
}

String capsuleTime(const CapsuleSummary &record) {
  char local[12];
  if (formatUtcOffsetShort(record.createdAt.c_str(),
                           kChinaStandardTimeOffsetMinutes,
                           local, sizeof(local))) {
    return String(local);
  }
  return record.createdAt.isEmpty() ? String("刚刚") : record.createdAt;
}

String durationLabel(uint32_t durationMs) {
  if (durationMs == 0) return "";
  return String((durationMs + 500) / 1000) + " 秒";
}

String capsuleScopeLabel(CapsuleScope scope) {
  switch (scope) {
    case CapsuleScope::inbox: return "收件箱";
    case CapsuleScope::favorites: return "收藏";
    case CapsuleScope::pending: return "待转写";
    case CapsuleScope::failed: return "失败";
    case CapsuleScope::archive: return "归档";
    case CapsuleScope::trash: return "回收站";
  }
  return "胶囊";
}

bool generatedVoiceTitle(const String &title) {
  return title.startsWith("语音 20") && title.indexOf('T') >= 0;
}

String capsuleDisplayText(const CapsuleSummary &record) {
  if (record.status == CapsuleStatus::queued && !record.error.isEmpty()) {
    return "等待网络重试";
  }
  if (!record.preview.isEmpty() &&
      !(record.preview == record.title && generatedVoiceTitle(record.title))) {
    return record.preview;
  }
  switch (record.status) {
    case CapsuleStatus::queued: return "等待转写";
    case CapsuleStatus::transcribing: return "正在转写";
    case CapsuleStatus::failed: return "转写失败";
    default: return "语音胶囊";
  }
}

uint16_t wifiColor(WifiPhase phase) {
  switch (phase) {
    case WifiPhase::online:
    case WifiPhase::grace: return ui::kAccent;
    case WifiPhase::connecting:
    case WifiPhase::provisioning: return ui::kWaiting;
    case WifiPhase::error: return ui::kError;
    case WifiPhase::off:
    case WifiPhase::disabled: return ui::kMuted;
  }
  return ui::kMuted;
}

uint16_t provisioningColor(ProvisioningState state) {
  switch (state) {
    case ProvisioningState::connected: return ui::kAccent;
    case ProvisioningState::connecting:
    case ProvisioningState::scanning: return ui::kWaiting;
    case ProvisioningState::error: return ui::kError;
    case ProvisioningState::ready: return ui::kMuted;
  }
  return ui::kMuted;
}

WirelessSyncPresentationInput wirelessSyncInput(const DashboardView &view) {
  WirelessSyncPresentationInput input;
  input.secureReady = view.wifiSyncSecureReady;
  input.paired = view.wifiSyncPaired;
  input.windowOpen = view.wifiSyncOpen;
  input.networkConnected = view.wifiSyncNetworkConnected;
  input.listenerActive = view.wifiSyncListener;
  input.bonjourActive = view.wifiSyncBonjour;
  input.clientConnected = view.wifiSyncClient;
  input.authenticated = view.wifiSyncAuthenticated;
  input.linkBusy = view.wifiSyncBusy;
  input.completed = view.wifiSyncCompleted;
  input.hasError = !view.wifiSyncLastError.isEmpty();
  input.remainingSeconds = view.wifiSyncRemainingSeconds;
  return input;
}

String syncClock(uint32_t seconds) {
  char value[16];
  snprintf(value, sizeof(value), "%lu:%02lu",
           static_cast<unsigned long>(seconds / 60),
           static_cast<unsigned long>(seconds % 60));
  return String(value);
}

String syncPhaseTitle(WirelessSyncPresentationPhase phase) {
  switch (phase) {
    case WirelessSyncPresentationPhase::unpaired: return "等待配对";
    case WirelessSyncPresentationPhase::idle: return "同步已关闭";
    case WirelessSyncPresentationPhase::opening: return "正在开启";
    case WirelessSyncPresentationPhase::waiting: return "等待 Mac";
    case WirelessSyncPresentationPhase::authenticating: return "正在认证";
    case WirelessSyncPresentationPhase::syncing: return "正在同步";
    case WirelessSyncPresentationPhase::completed: return "同步完成";
    case WirelessSyncPresentationPhase::failed: return "连接失败";
  }
  return "电脑同步";
}

uint16_t syncPhaseColor(WirelessSyncPresentationPhase phase) {
  switch (phase) {
    case WirelessSyncPresentationPhase::waiting:
    case WirelessSyncPresentationPhase::syncing:
    case WirelessSyncPresentationPhase::completed: return ui::kAccent;
    case WirelessSyncPresentationPhase::opening:
    case WirelessSyncPresentationPhase::authenticating: return ui::kWaiting;
    case WirelessSyncPresentationPhase::failed: return ui::kError;
    case WirelessSyncPresentationPhase::unpaired:
    case WirelessSyncPresentationPhase::idle: return ui::kMuted;
  }
  return ui::kMuted;
}

String syncErrorLabel(const String &error) {
  if (error == "network-unavailable") return "Wi-Fi 连接中断";
  if (error == "secure-server-unavailable") return "安全服务未就绪";
  if (error == "listener-start-failed") return "监听服务启动失败";
  if (error == "bonjour-start-failed") return "局域网发现启动失败";
  if (error == "tls-initialization-failed" || error == "tls-failed") {
    return "安全连接失败";
  }
  if (error == "authentication-failed") return "Mac 认证失败";
  if (error == "authentication-timeout") return "Mac 认证超时";
  if (error == "peer-closed" || error == "link-disconnected") {
    return "Mac 已断开";
  }
  return error.isEmpty() ? String("等待连接") : String("同步服务异常");
}

}  // namespace

void Dashboard::begin(Arduino_GFX *display, fs::FS *fs) {
  output_ = display;
  display_ = display;
  if (output_ != nullptr) {
    frame_ = new (std::nothrow) Arduino_Canvas_Indexed(
        ui::kScreenWidth, ui::kScreenHeight, output_);
    if (frame_ != nullptr && frame_->begin(GFX_SKIP_OUTPUT_BEGIN)) {
      display_ = frame_;
    } else {
      delete frame_;
      frame_ = nullptr;
    }
  }
  renderer_.begin(display_, fs);
  invalidated_ = true;
  if (display_ != nullptr) {
    display_->fillScreen(ui::kBackground);
    presentFrame();
  }
}

void Dashboard::draw(const DashboardView &view) {
  if (display_ == nullptr || view.board == nullptr) return;
  const uint32_t libraryRevision =
      view.library == nullptr ? 0 : view.library->revision();
  const bool libraryChanged = libraryRevision != lastLibraryRevision_;
  if (libraryChanged) {
    detailBodyCacheKey_ = "";
    reconcileCapsules(view.library);
    lastLibraryRevision_ = libraryRevision;
  }
  if (view.recording && !lastRecording_) {
    smoothedPeak_ = 0;
    envelopeCeiling_ = 1200;
    animationTick_ = 0;
  }
  state_.provisioning = view.provisioning;
  if (!view.provisioning) state_.provisioningLog = false;
  provisioningLogCount_ = static_cast<uint8_t>(
      view.provisioningDiagnostics == nullptr
          ? 0 : view.provisioningDiagnostics->count());
  if (provisioningLogScroll_.setMaximum(provisioningScrollMaximum())) {
    invalidated_ = true;
  }
  state_.capsuleSelectionMode = browserState_.selectionMode();
  state_.capsuleTrashScope = view.library != nullptr &&
      view.library->scope() == CapsuleScope::trash;
  state_.undoAvailable = view.undoAvailable;
  if ((view.recording || view.wirelessHolding) && !view.provisioning &&
      (state_.page != RootPage::home || state_.capsuleDetail ||
       state_.computerSync || state_.bluetoothPairing)) {
    state_.page = RootPage::home;
    state_.capsuleDetail = false;
    state_.computerSync = false;
    state_.bluetoothPairing = false;
    state_.capsuleScopeOverlay = false;
    state_.detailMoreOverlay = false;
    state_.purgeConfirmOverlay = false;
    purgeConfirmCount_ = 0;
    browserState_.clearFocus();
    invalidated_ = true;
  }
  state_.homeMode = view.recording ? HomeMode::recording :
      (view.transcribing ? HomeMode::transcribing : HomeMode::idle);
  const String currentSignature = signature(view, true);
  const String currentStableSignature = signature(view, false);
  bool bodyRepainted = false;
  if (invalidated_) {
    const bool scrollOnly = scrollFramePending_ && frame_ != nullptr &&
        !libraryChanged && !lastSignature_.isEmpty() &&
        currentStableSignature == lastStableSignature_;
    if (scrollOnly && composeAndPresentScrollFrame(view)) {
      ++partialRedrawCount_;
    } else {
      display_->fillScreen(ui::kBackground);
      if (state_.screen() == UiScreen::home ||
          state_.screen() == UiScreen::capsules ||
          state_.screen() == UiScreen::device) drawTopBar(view);
      drawBody(view);
      const UiRenderPlan plan = uiRenderPlan(view.recording, true);
      if (plan.composeRecordingBeforeFullPresent) {
        drawRecordingDynamic(view, false);
      }
      if (!beginPreparedPageTransition(millis())) presentFrame();
      ++fullRedrawCount_;
    }
    bodyRepainted = true;
    lastSignature_ = currentSignature;
    lastStableSignature_ = currentStableSignature;
    lastTopBarSignature_ = topBarSignature(view);
    lastWirelessHolding_ = view.wirelessHolding;
    invalidated_ = false;
    scrollFramePending_ = false;
  } else if (currentSignature != lastSignature_) {
    display_->fillScreen(ui::kBackground);
    if (state_.screen() == UiScreen::home ||
        state_.screen() == UiScreen::capsules ||
        state_.screen() == UiScreen::device) drawTopBar(view);
    drawBody(view);
    const UiRenderPlan plan = uiRenderPlan(view.recording, true);
    if (plan.composeRecordingBeforeFullPresent) {
      drawRecordingDynamic(view, false);
    }
    presentFrame();
    bodyRepainted = true;
    ++bodyRedrawCount_;
    lastSignature_ = currentSignature;
    lastStableSignature_ = currentStableSignature;
    lastTopBarSignature_ = topBarSignature(view);
    lastWirelessHolding_ = view.wirelessHolding;
  }
  drawDynamicRegions(view);
  const UiRenderPlan plan = uiRenderPlan(view.recording, bodyRepainted);
  if (state_.screen() == UiScreen::home && plan.presentRecordingAsPartial) {
    drawRecordingDynamic(view, true);
  }
  lastRecording_ = view.recording;
}

void Dashboard::drawBody(const DashboardView &view) {
  switch (state_.screen()) {
    case UiScreen::capsuleDetail: drawCapsuleDetail(view); break;
    case UiScreen::computerSync: drawComputerSync(view); break;
    case UiScreen::bluetoothPairing: drawBluetoothPairing(view); break;
    case UiScreen::provisioning: drawProvisioning(view); break;
    case UiScreen::provisioningLog: drawProvisioningLog(view); break;
    case UiScreen::home: drawHome(view); drawPageIndicator(); break;
    case UiScreen::capsules: drawCapsules(view); drawPageIndicator(); break;
    case UiScreen::device: drawDevice(view); drawPageIndicator(); break;
  }
  if (state_.capsuleScopeOverlay) drawScopePicker(view);
  if (state_.detailMoreOverlay) drawDetailMore(view);
  if (state_.purgeConfirmOverlay) drawPurgeConfirm();
  if (!state_.capsuleScopeOverlay && !state_.detailMoreOverlay &&
      !state_.purgeConfirmOverlay &&
      shouldDrawToast(!view.message.isEmpty(), view.recording,
                      view.wirelessHolding,
                      state_.screen() == UiScreen::provisioning)) {
    drawToast(view.message);
  }
}

void Dashboard::drawTopBar(const DashboardView &view) {
  const BoardStatus &board = *view.board;
  const uint16_t batteryColor = board.charging ? ui::kAccent : ui::kInk;
  display_->drawRoundRect(20, 16, 25, 12, 2, batteryColor);
  display_->fillRect(45, 19, 3, 6, batteryColor);
  if (board.batteryPercent >= 0) {
    const int16_t fill = static_cast<int16_t>(
        board.batteryPercent > 100 ? 21 : board.batteryPercent * 21 / 100);
    if (fill > 0) display_->fillRect(22, 18, fill, 8, batteryColor);
  }
  const String battery = board.batteryPercent >= 0
      ? String(board.batteryPercent) + "%" : String("--");
  renderer_.drawText(battery, 56, 13, 62, 1, ui::kInk, ui::kBackground);
  drawUiIcon(*display_, UiIcon::wifi, 238, 8, wifiColor(view.wifiPhase));
  if (wifiUiShowsDisconnectedSlash(view.wifiPhase)) {
    display_->drawLine(241, 11, 258, 28, wifiColor(view.wifiPhase));
  }
  if (view.transcribing) display_->fillCircle(272, 18, 4, ui::kWaiting);
  drawUiIcon(*display_, UiIcon::bluetooth, 282, 8,
             view.bleVoiceReady ? ui::kWireless : ui::kMuted);
  drawUiIcon(*display_, UiIcon::mac, 326, 8,
             view.usbConnected ? ui::kAccent : ui::kMuted);
}

void Dashboard::drawPageIndicator() {
  const RootPage pages[] = {
      RootPage::capsules, RootPage::home, RootPage::device};
  const int16_t left[] = {154, 177, 200};
  for (uint8_t index = 0; index < 3; ++index) {
    const bool active = state_.page == pages[index];
    display_->fillRoundRect(left[index], ui::kPageIndicatorTop,
                            active ? 14 : 8, 3, 2,
                            active ? ui::kAccent : ui::kDisabled);
  }
}

void Dashboard::drawBackButton() {
  drawUiIcon(*display_, UiIcon::back, 16, 16, ui::kAccent);
}

void Dashboard::drawHome(const DashboardView &view) {
  if (view.recording) {
    drawCenteredText("轻触停止", 350, UiTextSize::body, ui::kInk, true);
    return;
  }
  drawHomeAction(ui::kHomePrimaryTop, ui::kHomePrimaryConnectedBottom,
                 false, false);
  drawHomeAction(ui::kHomeSecondaryTop, ui::kHomeSecondaryBottom,
                 true, view.wirelessHolding, view.bleVoiceReady);
}

void Dashboard::drawHomeAction(int16_t top, int16_t bottom, bool wireless,
                               bool holding, bool enabled) {
  const uint16_t accent = wireless
      ? (enabled ? ui::kWireless : ui::kMuted) : ui::kAccent;
  const uint16_t dim = wireless ? ui::kWirelessDim : ui::kAccentDim;
  const uint16_t fill = holding ? dim : ui::kSurface;
  display_->fillRoundRect(20, top, 328, bottom - top, 28, fill);
  display_->drawRoundRect(20, top, 328, bottom - top, 28,
                          holding ? accent : ui::kDivider);
  const int16_t centerY = top + (bottom - top) / 2;
  if (wireless) {
    display_->drawCircle(78, centerY, 29, dim);
    display_->drawCircle(78, centerY, 20, accent);
    display_->drawFastVLine(78, centerY - 12, 24, accent);
    display_->drawFastHLine(68, centerY, 20, accent);
  } else {
    drawCapsuleMark(*display_, 78, centerY, 82, 42, accent, fill);
  }
  renderer_.drawText(wireless ? "微信语音输入" : "语音胶囊",
                     132, centerY - 34, 196, 1,
                     ui::kInk, fill, 0, false, UiTextSize::display, true);
  renderer_.drawText(wireless ? (holding ? "松开结束" :
                                  (enabled ? "按住说话" : "等待 Mac 应用")) :
                                  "轻触录音",
                     132, centerY + 10, 190, 1,
                     accent, fill, 0, false, UiTextSize::body, true);
}

void Dashboard::drawCapsules(const DashboardView &view) {
  const size_t count = view.library == nullptr ? 0 : view.library->count();
  const String scope = browserState_.selectionMode()
      ? String("已选 ") + browserState_.selectedCount() + " 条"
      : (view.library == nullptr ? String("收件箱")
                                 : capsuleScopeLabel(view.library->scope()));
  renderer_.drawText(scope, 20, 58, 180, 1, ui::kInk, ui::kBackground,
                     0, false, UiTextSize::display, true);
  const String countText = browserState_.selectionMode()
      ? String("完成") : String(count) + " 条";
  const int16_t countWidth = renderer_.measureTextWidth(countText);
  renderer_.drawText(countText, 348 - countWidth, 66, countWidth, 1,
                     ui::kMuted, ui::kBackground);
  if (count == 0) {
    drawCapsuleOrb(230, ui::kDisabled, ui::kSurfaceRaised, 88);
    drawCenteredText("暂无胶囊", 326, UiTextSize::body, ui::kMuted, true);
    return;
  }
  drawCapsuleRows(view);
  if (browserState_.selectionMode()) {
    display_->fillRoundRect(20, ui::kCapsuleSelectionBarTop, 328,
                            ui::kCapsuleSelectionBarBottom -
                                ui::kCapsuleSelectionBarTop,
                            18, ui::kSurfaceRaised);
    display_->drawFastVLine(123, ui::kCapsuleSelectionBarTop + 12, 48,
                            ui::kDivider);
    display_->drawFastVLine(245, ui::kCapsuleSelectionBarTop + 12, 48,
                            ui::kDivider);
    renderer_.drawText("收藏", 48, ui::kCapsuleSelectionBarTop + 25,
                       56, 1, ui::kInk, ui::kSurfaceRaised);
    renderer_.drawText(view.library != nullptr &&
                               view.library->scope() == CapsuleScope::trash
                           ? "恢复" :
                           (view.library != nullptr &&
                                view.library->scope() == CapsuleScope::archive
                                ? "移回" : "归档"),
                       157, ui::kCapsuleSelectionBarTop + 25,
                       64, 1, ui::kInk, ui::kSurfaceRaised);
    renderer_.drawText(view.library != nullptr &&
                               view.library->scope() == CapsuleScope::trash
                           ? "永久删除" : "删除",
                       278, ui::kCapsuleSelectionBarTop + 25,
                       70, 1, ui::kError, ui::kSurfaceRaised);
  }
}

void Dashboard::drawCapsuleRows(const DashboardView &view) {
  const size_t count = view.library == nullptr ? 0 : view.library->count();
  if (count == 0) return;
  const int16_t listBottom = browserState_.selectionMode()
      ? ui::kCapsuleSelectionBarTop : ui::kCapsuleListBottom;
  const int32_t scrollPx = capsuleScroll_.positionPx();
  const size_t firstIndex = static_cast<size_t>(
      scrollPx / ui::kCapsuleRowStride);
  const int16_t firstY = ui::kCapsuleListTop -
      static_cast<int16_t>(scrollPx % ui::kCapsuleRowStride);
  for (size_t index = firstIndex; index < count; ++index) {
    const int16_t y = firstY + static_cast<int16_t>(
        (index - firstIndex) * ui::kCapsuleRowStride);
    if (y >= listBottom) break;
    const CapsuleSummary *record = view.library->at(index);
    if (record == nullptr) break;
    const uint16_t stateColor = record->readOnly
        ? ui::kWaiting : capsuleStatusColor(record->status);
    if (browserState_.selectionMode() && y + 13 >= ui::kCapsuleListTop &&
        y + 13 < listBottom) {
      display_->drawCircle(25, y + 13, 9,
                           browserState_.selected(record->id.c_str())
                               ? ui::kAccent : ui::kMuted);
      if (browserState_.selected(record->id.c_str())) {
        display_->fillCircle(25, y + 13, 4, ui::kAccent);
      }
    } else if (!browserState_.selectionMode() &&
               y + 13 >= ui::kCapsuleListTop && y + 13 < listBottom) {
      display_->fillCircle(25, y + 13, 3, stateColor);
    }
    const String preview = capsuleDisplayText(*record);
    renderer_.drawText(preview, 42, y, 270, 1, ui::kInk, ui::kBackground,
                       0, true, UiTextSize::body, false, 0,
                       ui::kCapsuleListTop, listBottom);
    String metadata = capsuleTime(*record) + "  " +
        capsuleStatus(*record);
    const String duration = durationLabel(record->durationMs);
    if (!duration.isEmpty()) metadata += "  " + duration;
    renderer_.drawText(metadata, 42, y + 34, 276, 1,
                       stateColor, ui::kBackground, 0, false,
                       UiTextSize::compact, false, 0,
                       ui::kCapsuleListTop, listBottom);
    if (record->favorite && y >= ui::kCapsuleListTop && y + 24 < listBottom) {
      drawUiIcon(*display_, UiIcon::star, 322, y, ui::kWaiting);
    }
    if (y + 67 >= ui::kCapsuleListTop && y + 67 < listBottom) {
      display_->drawFastHLine(42, y + 67, 306, ui::kDivider);
    }
  }
}

void Dashboard::drawCapsuleDetail(const DashboardView &view) {
  drawBackButton();
  const CapsuleSummary *record = view.library == nullptr
      ? nullptr : selected(*view.library);
  if (record == nullptr) {
    drawCenteredText("胶囊需要检查", 190, UiTextSize::body, ui::kError, true);
    return;
  }
  const String status = record->readOnly ? String("版本过新 · 只读")
                                         : capsuleStatus(*record);
  const int16_t statusWidth = renderer_.measureTextWidth(status);
  renderer_.drawText(status, 348 - statusWidth, 20, statusWidth, 1,
                     capsuleStatusColor(record->status), ui::kBackground);
  renderer_.drawText(capsuleTime(*record), 20, 58, 180, 1,
                     ui::kMuted, ui::kBackground);
  drawCapsuleDetailText(view, *record);

  const bool needsRetry = !record->readOnly &&
      (record->status == CapsuleStatus::failed ||
       (record->status == CapsuleStatus::queued && !record->error.isEmpty())) &&
      !record->trashed;
  state_.detailRetryEnabled = needsRetry;
  state_.detailTrashEnabled = !record->readOnly &&
      record->status != CapsuleStatus::transcribing;
  const uint16_t mutationColor = record->readOnly ? ui::kDisabled : ui::kMuted;
  drawDetailAction(6, view.playing ? UiIcon::stop : UiIcon::play,
                   view.playing ? "停止" : "播放", !record->readOnly,
                   record->readOnly ? ui::kDisabled : ui::kAccent);
  drawDetailAction(98, UiIcon::star,
                   record->favorite ? "已收藏" : "收藏", false,
                   record->readOnly ? ui::kDisabled :
                       (record->favorite ? ui::kWaiting : ui::kMuted));
  drawDetailAction(190, UiIcon::archive,
                   record->trashed ? "恢复" :
                   (record->archived ? "移回" : "归档"), false,
                   mutationColor);
  drawDetailAction(282, UiIcon::chevron, "更多", false, mutationColor);
}

void Dashboard::drawCapsuleDetailText(const DashboardView &view,
                                      const CapsuleSummary &record) {
  const String bodyCacheKey = record.id + ":" +
      static_cast<int>(record.status) + ":" + record.preview;
  if (bodyCacheKey != detailBodyCacheKey_) {
    detailBodyCache_ = view.library->readBestText(record);
    if (detailBodyCache_ == record.title &&
        generatedVoiceTitle(record.title)) {
      detailBodyCache_ = capsuleDisplayText(record);
    }
    detailBodyCacheKey_ = bodyCacheKey;
  }
  const int16_t lineHeight = renderer_.textLineHeight(UiTextSize::body);
  const int32_t contentHeight = static_cast<int32_t>(
      renderer_.wrappedLineCount(detailBodyCache_, 328, UiTextSize::body)) *
      lineHeight;
  const int32_t viewportHeight =
      ui::kDetailTextBottom - ui::kDetailTextTop;
  detailScroll_.setMaximum(contentHeight > viewportHeight
                               ? contentHeight - viewportHeight : 0);
  const int32_t scrollPx = detailScroll_.positionPx();
  const uint16_t skipLines = static_cast<uint16_t>(scrollPx / lineHeight);
  const int16_t pixelOffset = static_cast<int16_t>(scrollPx % lineHeight);
  const uint8_t visibleLines = static_cast<uint8_t>(
      viewportHeight / lineHeight + 2);
  renderer_.drawText(detailBodyCache_, 20, ui::kDetailTextTop, 328,
                     visibleLines, ui::kInk, ui::kBackground, skipLines,
                     true, UiTextSize::body, false, pixelOffset,
                     ui::kDetailTextTop, ui::kDetailTextBottom);
}

void Dashboard::drawScopePicker(const DashboardView &view) {
  static const char *labels[] = {
      "收件箱", "收藏", "待转写", "失败", "归档", "回收站"};
  const CapsuleScope active = view.library == nullptr
      ? CapsuleScope::inbox : view.library->scope();
  display_->fillRoundRect(ui::kScopePickerLeft, ui::kScopePickerTop,
                          ui::kScopePickerRight - ui::kScopePickerLeft,
                          ui::kScopePickerBottom - ui::kScopePickerTop,
                          24, ui::kSurfaceRaised);
  display_->drawRoundRect(ui::kScopePickerLeft, ui::kScopePickerTop,
                          ui::kScopePickerRight - ui::kScopePickerLeft,
                          ui::kScopePickerBottom - ui::kScopePickerTop,
                          24, ui::kDivider);
  for (uint8_t index = 0; index < 6; ++index) {
    const int16_t top = ui::kScopePickerTop +
        index * ui::kScopePickerRowHeight;
    if (index > 0) {
      display_->drawFastHLine(ui::kScopePickerLeft + 20, top,
                              ui::kScopePickerRight -
                                  ui::kScopePickerLeft - 40,
                              ui::kDivider);
    }
    const bool selected = static_cast<uint8_t>(active) == index;
    renderer_.drawText(labels[index], ui::kScopePickerLeft + 28, top + 14,
                       220, 1, selected ? ui::kAccent : ui::kInk,
                       ui::kSurfaceRaised, 0, false, UiTextSize::body,
                       selected);
    if (selected) {
      drawUiIcon(*display_, UiIcon::check, ui::kScopePickerRight - 52,
                 top + 12, ui::kAccent);
    }
  }
}

void Dashboard::drawDetailMore(const DashboardView &view) {
  const CapsuleSummary *record = view.library == nullptr
      ? nullptr : selected(*view.library);
  if (record == nullptr) return;
  display_->fillRoundRect(ui::kDetailMoreLeft, ui::kDetailMoreTop,
                          ui::kDetailMoreRight - ui::kDetailMoreLeft,
                          ui::kDetailMoreBottom - ui::kDetailMoreTop,
                          24, ui::kSurfaceRaised);
  display_->drawRoundRect(ui::kDetailMoreLeft, ui::kDetailMoreTop,
                          ui::kDetailMoreRight - ui::kDetailMoreLeft,
                          ui::kDetailMoreBottom - ui::kDetailMoreTop,
                          24, ui::kDivider);
  const uint16_t retryColor = state_.detailRetryEnabled
      ? ui::kWaiting : ui::kDisabled;
  drawUiIcon(*display_, UiIcon::retry, ui::kDetailMoreLeft + 22,
             ui::kDetailMoreTop + 22, retryColor);
  renderer_.drawText("重新转写", ui::kDetailMoreLeft + 64,
                     ui::kDetailMoreTop + 22, 200, 1, retryColor,
                     ui::kSurfaceRaised, 0, false, UiTextSize::body, true);
  const int16_t dividerY = ui::kDetailMoreTop + ui::kDetailMoreRowHeight;
  display_->drawFastHLine(ui::kDetailMoreLeft + 20, dividerY,
                          ui::kDetailMoreRight - ui::kDetailMoreLeft - 40,
                          ui::kDivider);
  const uint16_t trashColor = state_.detailTrashEnabled
      ? ui::kError : ui::kDisabled;
  drawUiIcon(*display_, UiIcon::warning, ui::kDetailMoreLeft + 22,
             dividerY + 22, trashColor);
  renderer_.drawText(record->trashed ? "永久删除" : "移入回收站",
                     ui::kDetailMoreLeft + 64, dividerY + 22, 200, 1,
                     trashColor, ui::kSurfaceRaised, 0, false,
                     UiTextSize::body, true);
}

void Dashboard::drawPurgeConfirm() {
  display_->fillRoundRect(ui::kPurgeConfirmLeft, ui::kPurgeConfirmTop,
                          ui::kPurgeConfirmRight - ui::kPurgeConfirmLeft,
                          ui::kPurgeConfirmBottom - ui::kPurgeConfirmTop,
                          26, ui::kSurfaceRaised);
  display_->drawRoundRect(ui::kPurgeConfirmLeft, ui::kPurgeConfirmTop,
                          ui::kPurgeConfirmRight - ui::kPurgeConfirmLeft,
                          ui::kPurgeConfirmBottom - ui::kPurgeConfirmTop,
                          26, ui::kError);
  drawUiIcon(*display_, UiIcon::warning, 52, 158, ui::kError);
  renderer_.drawText(String("永久删除 ") + purgeConfirmCount_ + " 条？",
                     92, 154, 220, 1, ui::kInk, ui::kSurfaceRaised,
                     0, false, UiTextSize::display, true);
  renderer_.drawText("录音和文字将无法恢复", 52, 212, 264, 2,
                     ui::kMuted, ui::kSurfaceRaised, 0, true,
                     UiTextSize::body, false);
  display_->drawFastHLine(48, ui::kPurgeConfirmActionsTop, 272, ui::kDivider);
  display_->drawFastVLine(ui::kPurgeConfirmActionSplit,
                          ui::kPurgeConfirmActionsTop, 54, ui::kDivider);
  renderer_.drawText("取消", 82, 294, 72, 1, ui::kInk,
                     ui::kSurfaceRaised, 0, false, UiTextSize::body, true);
  renderer_.drawText("永久删除", 214, 294, 100, 1, ui::kError,
                     ui::kSurfaceRaised, 0, false, UiTextSize::body, true);
}

void Dashboard::drawDevice(const DashboardView &view) {
  renderer_.drawText("设备", 20, 58, 150, 1, ui::kInk, ui::kBackground,
                     0, false, UiTextSize::display, true);
  DeviceHealthState health;
  health.ioExpander = view.board->ioExpander;
  health.display = view.board->display;
  health.touch = view.board->touch;
  health.sdCard = view.board->sdCard;
  health.rtc = view.board->rtc;
  health.imu = view.board->imu;
  health.pmu = view.board->pmu;
  health.audio = view.audioReady;
  health.usb = view.usbReady;
  health.fullTextFont = renderer_.sdFontReady();
  const String healthText = health.ready() ? "硬件正常" : "硬件需检查";
  const int16_t healthWidth = renderer_.measureTextWidth(healthText);
  renderer_.drawText(healthText, 348 - healthWidth, 66, healthWidth, 1,
                     health.ready() ? ui::kAccent : ui::kError,
                     ui::kBackground);

  const bool wifiEnabled = view.settings != nullptr &&
      view.settings->wifiEnabled;
  drawSettingRow(ui::kDeviceWifiTop, UiIcon::wifi, "无线网络",
                 wifiUiDetail(view.wifiPhase, wifiEnabled),
                 wifiColor(view.wifiPhase),
                 SettingAccessory::toggle,
                 wifiUiSwitchOn(view.wifiPhase));
  char pairingLabel[24];
  snprintf(pairingLabel, sizeof(pairingLabel), "配对码 %06lu",
           static_cast<unsigned long>(view.bleVoicePasskey));
  String voiceDetail;
  if (view.bleVoicePairing) {
    voiceDetail = pairingLabel;
  } else if (view.bleVoiceReady) {
    const uint32_t issues = view.bleVoiceNotifyFailures +
        view.bleVoiceQueueOverflows + view.bleVoiceReadyTimeouts +
        view.bleVoiceStopAckTimeouts + view.bleVoiceStreamTimeouts;
    voiceDetail = issues == 0
        ? String("就绪 · MTU") + String(view.bleVoiceMtu)
        : String("就绪 · 异常 ") + String(issues);
  } else if (view.bleVoiceConnected) {
    voiceDetail = String("质量不足 · ") + String(view.bleVoiceMtu);
  } else {
    voiceDetail = view.bleVoiceBonded ? "等待 Mac" : "轻触配对";
  }
  drawSettingRow(ui::kDeviceMacTop, UiIcon::bluetooth, "蓝牙配对",
                 voiceDetail,
                 view.bleVoiceReady || view.bleVoicePairing
                     ? ui::kWireless : ui::kMuted,
                 SettingAccessory::value);
  const WirelessSyncPresentationPhase syncPhase =
      wirelessSyncPresentationPhase(wirelessSyncInput(view));
  String syncDetail = syncPhaseTitle(syncPhase);
  if (view.wifiSyncOpen) {
    syncDetail += String(" · ") + syncClock(view.wifiSyncRemainingSeconds);
  } else {
    syncDetail = "轻触开启 5 分钟";
  }
  drawSettingRow(ui::kDeviceStorageTop, UiIcon::mac, "与电脑同步",
                 syncDetail, syncPhaseColor(syncPhase),
                 SettingAccessory::chevron);
  const bool raiseEnabled = view.settings != nullptr &&
      view.settings->raiseToWake;
  drawSettingRow(ui::kDeviceRaiseTop, UiIcon::raise, "自动亮屏",
                 raiseEnabled ? "触摸或抬起" : "仅实体键",
                 raiseEnabled ? ui::kAccent : ui::kMuted,
                 SettingAccessory::toggle, raiseEnabled);
  drawSettingRow(ui::kDeviceProvisionTop, UiIcon::phone, "手机配网",
                 "", ui::kMuted, SettingAccessory::chevron);
}

void Dashboard::drawComputerSync(const DashboardView &view) {
  drawBackButton();
  renderer_.drawText("电脑同步", 64, 18, 220, 1, ui::kInk,
                     ui::kBackground, 0, false, UiTextSize::body, true);

  const WirelessSyncPresentationPhase phase =
      wirelessSyncPresentationPhase(wirelessSyncInput(view));
  const uint16_t color = syncPhaseColor(phase);
  display_->fillRoundRect(20, 72, 328, 108, 24, ui::kSurface);
  display_->drawRoundRect(20, 72, 328, 108, 24, color);
  drawUiIcon(*display_, phase == WirelessSyncPresentationPhase::failed
                           ? UiIcon::warning : UiIcon::mac,
             42, 92, color);
  renderer_.drawText(syncPhaseTitle(phase), 82, 86, 238, 1, ui::kInk,
                     ui::kSurface, 0, false, UiTextSize::display, true);
  String detail;
  switch (phase) {
    case WirelessSyncPresentationPhase::unpaired:
      detail = "请用 USB 在 Mac 完成配对";
      break;
    case WirelessSyncPresentationPhase::idle:
      detail = "从顶栏一击开启五分钟";
      break;
    case WirelessSyncPresentationPhase::opening:
      detail = view.wifiSyncNetworkConnected
          ? "正在启动局域网安全服务" : "正在连接 Wi-Fi";
      break;
    case WirelessSyncPresentationPhase::waiting:
      detail = view.wifiSyncAuthenticated
          ? "Mac 已认证，等待同步事务" : "等待同一网络的 Mac";
      break;
    case WirelessSyncPresentationPhase::authenticating:
      detail = "正在验证已配对的 Mac";
      break;
    case WirelessSyncPresentationPhase::syncing:
      detail = "正在传输和提交胶囊";
      break;
    case WirelessSyncPresentationPhase::completed:
      detail = "本轮同步事务已经完成";
      break;
    case WirelessSyncPresentationPhase::failed:
      detail = syncErrorLabel(view.wifiSyncLastError);
      break;
  }
  renderer_.drawText(detail, 42, 132, 284, 2, color, ui::kSurface,
                     0, true, UiTextSize::compact, false);

  const int16_t rows[] = {198, 246, 294};
  const UiIcon icons[] = {UiIcon::mac, UiIcon::wifi, UiIcon::retry};
  const String labels[] = {"USB", "Wi-Fi", "剩余时间"};
  const String values[] = {
      view.usbConnected ? String("已连接") : String("未连接"),
      view.wifiSyncNetworkConnected ? String("已连接")
                                    : String("未连接"),
      view.wifiSyncOpen ? syncClock(view.wifiSyncRemainingSeconds)
                        : String("--:--")};
  const uint16_t colors[] = {
      view.usbConnected ? ui::kAccent : ui::kMuted,
      view.wifiSyncNetworkConnected ? ui::kAccent : ui::kMuted,
      view.wifiSyncOpen ? ui::kWaiting : ui::kMuted};
  for (uint8_t index = 0; index < 3; ++index) {
    drawUiIcon(*display_, icons[index], 24, rows[index], colors[index]);
    renderer_.drawText(labels[index], 60, rows[index] + 2, 130, 1,
                       ui::kMuted, ui::kBackground, 0, false,
                       UiTextSize::body, true);
    const int16_t width = renderer_.measureTextWidth(values[index]);
    renderer_.drawText(values[index], 344 - width, rows[index] + 2,
                       width, 1, colors[index], ui::kBackground, 0,
                       false, UiTextSize::body, true);
    display_->drawFastHLine(60, rows[index] + 34, 284, ui::kDivider);
  }

  display_->fillRoundRect(20, ui::kComputerSyncCloseTop, 328,
                          ui::kComputerSyncCloseBottom -
                              ui::kComputerSyncCloseTop,
                          20, ui::kSurfaceRaised);
  display_->drawRoundRect(20, ui::kComputerSyncCloseTop, 328,
                          ui::kComputerSyncCloseBottom -
                              ui::kComputerSyncCloseTop,
                          20, view.wifiSyncOpen ? ui::kError : ui::kDivider);
  const String closeLabel = view.wifiSyncOpen ? "关闭同步" : "返回";
  const int16_t closeWidth = renderer_.measureTextWidth(
      closeLabel, UiTextSize::body);
  renderer_.drawText(closeLabel,
                     (ui::kScreenWidth - closeWidth) / 2,
                     ui::kComputerSyncCloseTop + 22, closeWidth, 1,
                     view.wifiSyncOpen ? ui::kError : ui::kInk,
                     ui::kSurfaceRaised, 0, false, UiTextSize::body, true);
}

void Dashboard::drawBluetoothPairing(const DashboardView &view) {
  drawBackButton();
  renderer_.drawText("蓝牙配对", 64, 18, 220, 1, ui::kInk,
                     ui::kBackground, 0, false, UiTextSize::body, true);

  String status;
  uint16_t statusColor = ui::kMuted;
  if (view.bleVoiceReady) {
    status = "已连接 · 可以语音输入";
    statusColor = ui::kWireless;
  } else if (view.bleVoiceConnected) {
    status = "已连接 · 质量不足";
    statusColor = ui::kWaiting;
  } else if (view.bleVoiceBonded) {
    status = "已配对 · 等待 Mac";
  } else {
    status = "尚未配对";
  }
  display_->fillRoundRect(20, 72, 328, 62, 18, ui::kSurface);
  drawUiIcon(*display_, UiIcon::bluetooth, 38, 91, statusColor);
  renderer_.drawText(status, 76, 91, 248, 1, statusColor, ui::kSurface,
                     0, false, UiTextSize::body, true);

  display_->fillRoundRect(20, ui::kBluetoothPairTop, 328,
                          ui::kBluetoothPairBottom - ui::kBluetoothPairTop,
                          20, ui::kSurfaceRaised);
  display_->drawRoundRect(20, ui::kBluetoothPairTop, 328,
                          ui::kBluetoothPairBottom - ui::kBluetoothPairTop,
                          20, view.bleVoicePairing ? ui::kWaiting
                                                   : ui::kWireless);
  renderer_.drawText(view.bleVoicePairing ? "取消配对" : "开始配对",
                     40, ui::kBluetoothPairTop + 15, 200, 1, ui::kInk,
                     ui::kSurfaceRaised, 0, false, UiTextSize::body, true);
  const String pairingDetail = view.bleVoicePairing
      ? String("配对码 ") + String(view.bleVoicePasskey)
      : String("两分钟内连接 PokePod Voice");
  renderer_.drawText(pairingDetail, 40, ui::kBluetoothPairTop + 48, 280, 1,
                     view.bleVoicePairing ? ui::kWaiting : ui::kMuted,
                     ui::kSurfaceRaised);

  const uint16_t forgetColor = view.bleVoiceBonded ? ui::kError
                                                    : ui::kDisabled;
  display_->fillRoundRect(20, ui::kBluetoothForgetTop, 328,
                          ui::kBluetoothForgetBottom -
                              ui::kBluetoothForgetTop,
                          20, ui::kSurfaceRaised);
  display_->drawRoundRect(20, ui::kBluetoothForgetTop, 328,
                          ui::kBluetoothForgetBottom -
                              ui::kBluetoothForgetTop,
                          20, forgetColor);
  renderer_.drawText("忘记 Mac", 40, ui::kBluetoothForgetTop + 15, 200, 1,
                     forgetColor, ui::kSurfaceRaised, 0, false,
                     UiTextSize::body, true);
  renderer_.drawText(view.bleVoiceBonded ? "清除已保存的电脑"
                                         : "当前没有已配对电脑",
                     40, ui::kBluetoothForgetTop + 48, 280, 1,
                     ui::kMuted, ui::kSurfaceRaised);
}

void Dashboard::drawProvisioning(const DashboardView &view) {
  drawBackButton();
  renderer_.drawText("连接手机", 64, 18, 220, 1, ui::kInk, ui::kBackground,
                     0, false, UiTextSize::body, true);
  const uint16_t statusColor = provisioningColor(view.portalState);
  display_->fillCircle(24, 68, 4, statusColor);
  renderer_.drawText(view.portalStatus, 38, 56, 310, 2,
                     statusColor, ui::kBackground);
  renderer_.drawText("热点名称", 20, 104, 180, 1,
                     ui::kMuted, ui::kBackground);
  display_->fillRoundRect(20, 128, 328, 70, 20, ui::kSurface);
  renderer_.drawText(view.portalSsid, 38, 150, 292, 1,
                     ui::kInk, ui::kSurface, 0, false, UiTextSize::body, true);
  renderer_.drawText("密码", 20, 214, 100, 1,
                     ui::kMuted, ui::kBackground);
  display_->fillRoundRect(20, 238, 328, 88, 20, ui::kSurface);
  renderer_.drawText(view.portalPassword, 38, 266, 292, 1,
                     ui::kWaiting, ui::kSurface, 0, false,
                     UiTextSize::display, true);
  display_->fillRoundRect(20, ui::kProvisionExitTop, 154,
                          ui::kProvisionExitBottom - ui::kProvisionExitTop,
                          18, ui::kSurfaceRaised);
  display_->drawRoundRect(20, ui::kProvisionExitTop, 154,
                          ui::kProvisionExitBottom - ui::kProvisionExitTop,
                          18, ui::kDivider);
  renderer_.drawText("诊断记录", 48, ui::kProvisionExitTop + 20, 100, 1,
                     ui::kAccent, ui::kSurfaceRaised, 0, false,
                     UiTextSize::body, true);
  display_->fillRoundRect(194, ui::kProvisionExitTop, 154,
                          ui::kProvisionExitBottom - ui::kProvisionExitTop,
                          18, ui::kSurfaceRaised);
  display_->drawRoundRect(194, ui::kProvisionExitTop, 154,
                          ui::kProvisionExitBottom - ui::kProvisionExitTop,
                          18, ui::kDivider);
  renderer_.drawText("退出配网", 222, ui::kProvisionExitTop + 20, 100, 1,
                     ui::kInk, ui::kSurfaceRaised, 0, false,
                     UiTextSize::body, true);
}

void Dashboard::drawProvisioningLog(const DashboardView &view) {
  drawBackButton();
  renderer_.drawText("配网诊断", 64, 18, 220, 1, ui::kInk, ui::kBackground,
                     0, false, UiTextSize::body, true);
  const uint16_t statusColor = provisioningColor(view.portalState);
  display_->fillCircle(24, 66, 4, statusColor);
  renderer_.drawText(view.portalStatus, 38, 54, 310, 2,
                     statusColor, ui::kBackground);
  const ProvisioningDiagnostics *diagnostics = view.provisioningDiagnostics;
  const size_t count = diagnostics == nullptr ? 0 : diagnostics->count();
  if (count == 0) {
    drawCenteredText("暂无配网记录", 220, UiTextSize::body, ui::kMuted, true);
    return;
  }
  drawProvisioningRows(view);
}

void Dashboard::drawProvisioningRows(const DashboardView &view) {
  const ProvisioningDiagnostics *diagnostics = view.provisioningDiagnostics;
  const size_t count = diagnostics == nullptr ? 0 : diagnostics->count();
  if (count == 0) return;
  const int32_t scrollPx = provisioningLogScroll_.positionPx();
  const size_t firstOffset = static_cast<size_t>(
      scrollPx / ui::kProvisionLogRowStride);
  const int16_t firstTop = ui::kProvisionLogListTop -
      static_cast<int16_t>(scrollPx % ui::kProvisionLogRowStride);
  for (size_t offset = firstOffset; offset < count; ++offset) {
    const StoredProvisioningLogRecord *record = diagnostics->newest(offset);
    if (record == nullptr) break;
    const int16_t top = firstTop + static_cast<int16_t>(
        (offset - firstOffset) * ui::kProvisionLogRowStride);
    if (top >= ui::kProvisionLogListBottom) break;
    const bool failed = record->outcome ==
        static_cast<uint8_t>(ProvisioningLogOutcome::failure);
    const uint16_t color = failed ? ui::kError :
        (record->outcome ==
             static_cast<uint8_t>(ProvisioningLogOutcome::success)
             ? ui::kAccent : ui::kMuted);
    const ProvisioningLogStage stage =
        static_cast<ProvisioningLogStage>(record->stage);
    if (top + 12 >= ui::kProvisionLogListTop &&
        top + 12 < ui::kProvisionLogListBottom) {
      display_->fillCircle(24, top + 12, 4, color);
    }
    renderer_.drawText(provisioningLogStageLabel(stage), 40, top, 238, 1,
                       ui::kInk, ui::kBackground, 0, true,
                       UiTextSize::body, true, 0,
                       ui::kProvisionLogListTop,
                       ui::kProvisionLogListBottom);
    renderer_.drawText(String("#") + record->sequence, 288, top + 2, 60, 1,
                       ui::kMuted, ui::kBackground, 0, false,
                       UiTextSize::compact, false, 0,
                       ui::kProvisionLogListTop,
                       ui::kProvisionLogListBottom);
    const String reason = provisioningLogReasonLabel(*record);
    if (record->ssid[0] != '\0') {
      renderer_.drawText(String(record->ssid), 40, top + 34, 300, 1, color,
                         ui::kBackground, 0, false, UiTextSize::body,
                         true, 0, ui::kProvisionLogListTop,
                         ui::kProvisionLogListBottom);
      renderer_.drawText(reason, 40, top + 63, 300, 1, color,
                         ui::kBackground, 0, false, UiTextSize::compact,
                         false, 0, ui::kProvisionLogListTop,
                         ui::kProvisionLogListBottom);
    } else {
      renderer_.drawText(reason, 40, top + 42, 300, 2, color,
                         ui::kBackground, 0, false, UiTextSize::compact,
                         false, 0, ui::kProvisionLogListTop,
                         ui::kProvisionLogListBottom);
    }
    if (top + 86 >= ui::kProvisionLogListTop &&
        top + 86 < ui::kProvisionLogListBottom) {
      display_->drawFastHLine(40, top + 86, 308, ui::kDivider);
    }
  }
}

void Dashboard::drawCapsuleOrb(int16_t centerY, uint16_t accent,
                               uint16_t dimAccent, int16_t scale) {
  const int16_t outerX = 108 * scale / 100;
  const int16_t outerY = 76 * scale / 100;
  display_->drawEllipse(184, centerY, outerX, outerY, ui::kSurfaceRaised);
  display_->drawEllipse(184, centerY, 92 * scale / 100,
                        64 * scale / 100, dimAccent);
  drawCapsuleMark(*display_, 184, centerY, 112 * scale / 100,
                  56 * scale / 100, accent, ui::kSurface);
}

void Dashboard::drawToast(const String &message) {
  const bool warning = message.indexOf("失败") >= 0 ||
      message.indexOf("异常") >= 0 || message.indexOf("检查") >= 0 ||
      message.indexOf("尚未") >= 0 || message.indexOf("无法") >= 0 ||
      message.indexOf("版本过新") >= 0 || message.indexOf("请插入") >= 0;
  const uint16_t statusColor = warning ? ui::kError : ui::kAccent;
  display_->fillRoundRect(20, 366, 328, 52, 18, ui::kSurfaceRaised);
  drawUiIcon(*display_, warning ? UiIcon::warning : UiIcon::check,
             34, 380, statusColor);
  renderer_.drawText(message, 70, 382, 258, 1,
                     ui::kInk, ui::kSurfaceRaised, 0, false,
                     UiTextSize::compact, true);
}

void Dashboard::drawCenteredText(const String &text, int16_t y,
                                 UiTextSize size, uint16_t color,
                                 bool bold, int16_t maxWidth) {
  const int16_t measured = renderer_.measureTextWidth(text, size);
  const int16_t width = measured < maxWidth ? measured : maxWidth;
  renderer_.drawText(text, (ui::kScreenWidth - width) / 2, y, width, 1,
                     color, ui::kBackground, 0, false, size, bold);
}

void Dashboard::drawSettingRow(int16_t top, UiIcon icon,
                               const String &title, const String &detail,
                               uint16_t detailColor,
                               SettingAccessory accessory,
                               bool toggleEnabled) {
  const bool twoLines = accessory == SettingAccessory::toggle &&
      !detail.isEmpty();
  drawUiIcon(*display_, icon, ui::kSettingIconLeft,
             top + ui::kSettingIconTopOffset, detailColor);
  renderer_.drawText(
      title, ui::kSettingTextLeft,
      top + (twoLines ? ui::kSettingTwoLineTitleTopOffset
                      : ui::kSettingSingleTitleTopOffset),
      accessory == SettingAccessory::value
          ? ui::kSettingTitleWithValueWidth : ui::kSettingTitleWidth, 1,
                     ui::kInk, ui::kBackground, 0, false,
                     UiTextSize::body, true);
  if (twoLines) {
    renderer_.drawText(detail, ui::kSettingTextLeft,
                       top + ui::kSettingDetailTopOffset,
                       ui::kSettingTitleWidth, 1,
                       detailColor, ui::kBackground);
  } else if (accessory == SettingAccessory::value && !detail.isEmpty()) {
    const int16_t width = renderer_.measureTextWidth(detail);
    renderer_.drawText(detail, ui::kSettingValueRight - width,
                       top + ui::kSettingValueTopOffset, width, 1,
                       detailColor, ui::kBackground);
  }
  if (accessory == SettingAccessory::toggle) {
    drawToggle(*display_, ui::kSettingTrailingLeft,
               top + ui::kSettingIconTopOffset, toggleEnabled,
               ui::kAccent, ui::kDisabled, ui::kBackground);
  } else if (accessory == SettingAccessory::chevron) {
    drawUiIcon(*display_, UiIcon::chevron,
               ui::kSettingValueRight - ui::kActionIconSize,
               top + ui::kSettingIconTopOffset, ui::kMuted);
  }
  display_->drawFastHLine(ui::kSettingTextLeft, top + 63,
                          ui::kScreenWidth - ui::kPageMargin -
                              ui::kSettingTextLeft,
                          ui::kDivider);
}

void Dashboard::drawDetailAction(int16_t left, UiIcon icon,
                                 const String &label, bool emphasized,
                                 uint16_t accent) {
  const uint16_t fill = emphasized ? ui::kAccentDim : ui::kSurface;
  display_->fillRoundRect(left, ui::kDetailActionsTop, 80,
                          ui::kDetailActionsBottom - ui::kDetailActionsTop,
                          18, fill);
  if (emphasized) {
    display_->drawRoundRect(left, ui::kDetailActionsTop, 80,
                            ui::kDetailActionsBottom - ui::kDetailActionsTop,
                            18, accent);
  }
  drawUiIcon(*display_, icon, left + 28, ui::kDetailActionsTop + 8, accent);
  const int16_t measured = renderer_.measureTextWidth(label);
  renderer_.drawText(label, left + (80 - measured) / 2,
                     ui::kDetailActionsTop + 42, measured, 1,
                     emphasized ? ui::kInk : accent, fill);
}

void Dashboard::drawRecordingDynamic(const DashboardView &view,
                                     bool presentPartial) {
  display_->fillRect(20, ui::kRecordingDynamicTop, 328,
                     ui::kRecordingDynamicBottom - ui::kRecordingDynamicTop,
                     ui::kBackground);
  smoothedPeak_ = UiMotionPolicy::smoothPeak(smoothedPeak_, view.audioPeak);
  const uint16_t frameMaximum = UiMotionPolicy::frameMaximum(
      view.audioEnvelope, PeakWindow::kEnvelopeSamples);
  envelopeCeiling_ = UiMotionPolicy::trackCeiling(envelopeCeiling_,
                                                  frameMaximum);

  const uint8_t breath = UiMotionPolicy::breath(animationTick_);
  const uint8_t energy = UiMotionPolicy::energy(smoothedPeak_,
                                                envelopeCeiling_);
  const int16_t centerX = 184;
  const int16_t centerY = 224;
  display_->drawEllipse(centerX, centerY, 122 + breath / 2 + energy,
                        74 + breath / 3 + energy / 2, ui::kAccentDim);
  display_->drawEllipse(centerX, centerY, 108 + energy / 2,
                        62 + energy / 3, ui::kDivider);
  display_->fillRoundRect(52, centerY - 48, 264, 96, 48, ui::kSurface);
  display_->drawRoundRect(52, centerY - 48, 264, 96, 48, ui::kAccent);

  constexpr int16_t kBarStride = 10;
  constexpr int16_t kBarWidth = 5;
  const int16_t firstX = centerX -
      static_cast<int16_t>(PeakWindow::kEnvelopeSamples * kBarStride) / 2;
  for (size_t index = 0; index < PeakWindow::kEnvelopeSamples; ++index) {
    const uint16_t sample = view.audioEnvelope[index];
    const int16_t height = UiMotionPolicy::barHeight(sample,
                                                     envelopeCeiling_);
    display_->fillRoundRect(firstX + index * kBarStride,
                            centerY - height / 2, kBarWidth, height, 2,
                            sample == 0 ? ui::kAccentDim : ui::kAccent);
  }

  const uint32_t second = view.recordingMs / 1000;
  char timer[12];
  snprintf(timer, sizeof(timer), "%02lu:%02lu",
           static_cast<unsigned long>(second / 60),
           static_cast<unsigned long>(second % 60));
  const String timerText(timer);
  const int16_t timerWidth = renderer_.measureTextWidth(timerText,
                                                        UiTextSize::timer);
  const int16_t timerX = (ui::kScreenWidth - timerWidth) / 2;
  display_->fillCircle(timerX - 14, 95, 4, ui::kError);
  renderer_.drawText(timerText, timerX, 78, timerWidth, 1,
                     ui::kInk, ui::kBackground, 0, false,
                     UiTextSize::timer, false);
  ++animationTick_;
  if (presentPartial) {
    presentRegion(20, ui::kRecordingDynamicTop, 328,
                  ui::kRecordingDynamicBottom - ui::kRecordingDynamicTop);
    ++partialRedrawCount_;
  }
}

void Dashboard::drawDynamicRegions(const DashboardView &view) {
  const UiScreen screen = state_.screen();
  if (screen == UiScreen::capsuleDetail ||
      screen == UiScreen::computerSync ||
      screen == UiScreen::bluetoothPairing ||
      screen == UiScreen::provisioning ||
      screen == UiScreen::provisioningLog) {
    return;
  }
  const String currentTopBar = topBarSignature(view);
  if (currentTopBar != lastTopBarSignature_) {
    display_->fillRect(0, 0, ui::kScreenWidth, ui::kTopBarHeight,
                       ui::kBackground);
    drawTopBar(view);
    presentRegion(0, 0, ui::kScreenWidth, ui::kTopBarHeight);
    lastTopBarSignature_ = currentTopBar;
    ++partialRedrawCount_;
  }
  if (screen == UiScreen::home && state_.homeMode != HomeMode::recording &&
      view.wirelessHolding != lastWirelessHolding_) {
    drawHomeAction(ui::kHomeSecondaryTop, ui::kHomeSecondaryBottom,
                   true, view.wirelessHolding, view.bleVoiceReady);
    presentRegion(20, ui::kHomeSecondaryTop, 328,
                  ui::kHomeSecondaryBottom - ui::kHomeSecondaryTop);
    lastWirelessHolding_ = view.wirelessHolding;
    ++partialRedrawCount_;
  }
}

void Dashboard::presentFrame() {
  if (frame_ != nullptr) frame_->flush();
}

bool Dashboard::beginPreparedPageTransition(uint32_t nowMs) {
  if (frame_ == nullptr || !pageTransition_.pending()) return false;
  const PageTransitionRegion region = pageTransition_.begin(
      nowMs, ui::kScreenWidth);
  if (!region.valid()) return false;
  presentRegion(region.x, 0, region.width, ui::kScreenHeight);
  return true;
}

bool Dashboard::advancePageTransition(uint32_t nowMs) {
  const PageTransitionRegion region = pageTransition_.advance(nowMs);
  if (!region.valid()) return false;
  presentRegion(region.x, 0, region.width, ui::kScreenHeight);
  ++partialRedrawCount_;
  return true;
}

bool Dashboard::composeAndPresentScrollFrame(const DashboardView &view) {
  const UiPresentRegion region = activeScrollPresentRegion();
  if (!region.valid()) return false;
  // Keep the top bar and fixed actions intact. Only the clipped viewport is
  // cleared, recomposed in the indexed canvas and transferred to the panel.
  display_->fillRect(region.x, region.y, region.width, region.height,
                     ui::kBackground);
  if (!drawActiveScrollSurface(view)) return false;
  presentRegion(region.x, region.y, region.width, region.height);
  return true;
}

bool Dashboard::drawActiveScrollSurface(const DashboardView &view) {
  switch (state_.screen()) {
    case UiScreen::capsules:
      drawCapsuleRows(view);
      return true;
    case UiScreen::capsuleDetail: {
      const CapsuleSummary *record = view.library == nullptr
          ? nullptr : selected(*view.library);
      if (record == nullptr) return false;
      drawCapsuleDetailText(view, *record);
      return true;
    }
    case UiScreen::provisioningLog:
      drawProvisioningRows(view);
      return true;
    default:
      return false;
  }
}

void Dashboard::presentScrollRegion() {
  const UiPresentRegion region = activeScrollPresentRegion();
  if (region.valid()) {
    presentRegion(region.x, region.y, region.width, region.height);
  } else {
    presentFrame();
  }
}

UiPresentRegion Dashboard::activeScrollPresentRegion() const {
  UiScrollSurface surface = UiScrollSurface::none;
  switch (state_.screen()) {
    case UiScreen::capsules: surface = UiScrollSurface::capsules; break;
    case UiScreen::capsuleDetail: surface = UiScrollSurface::detail; break;
    case UiScreen::provisioningLog:
      surface = UiScrollSurface::provisioningLog;
      break;
    default: break;
  }
  return scrollPresentRegion(surface, browserState_.selectionMode());
}

void Dashboard::presentRegion(int16_t x, int16_t y,
                              int16_t width, int16_t height) {
  if (frame_ == nullptr || output_ == nullptr) return;
  uint8_t *pixels = frame_->getFramebuffer() +
      static_cast<int32_t>(y) * ui::kScreenWidth + x;
  output_->drawIndexedBitmap(x, y, pixels, frame_->getColorIndex(),
                             width, height, ui::kScreenWidth - width);
}

String Dashboard::signature(const DashboardView &view,
                            bool includeScroll) const {
  String value;
  value.reserve(320);
  value += static_cast<int>(state_.screen());
  value += ':';
  value += static_cast<int>(state_.page);
  value += ':';
  value += browserState_.focusedId().c_str();
  value += ':';
  value += includeScroll ? capsuleScroll_.positionPx() : 0;
  value += ':';
  value += includeScroll ? detailScroll_.positionPx() : 0;
  value += ':';
  value += view.recording;
  value += ':';
  value += view.transcribing;
  value += ':';
  value += view.playing;
  value += ':';
  value += view.usbConnected;
  value += ':';
  value += view.bleVoiceConnected;
  value += ':';
  value += view.bleVoiceReady;
  value += ':';
  value += view.bleVoiceBonded;
  value += ':';
  value += view.bleVoicePairing;
  value += ':';
  value += view.bleVoicePasskey;
  value += ':';
  value += view.bleVoiceMtu;
  value += ':';
  value += view.bleVoiceNotifyFailures;
  value += ':';
  value += view.bleVoiceQueueOverflows;
  value += ':';
  value += view.bleVoiceReadyTimeouts;
  value += ':';
  value += view.bleVoiceStopAckTimeouts;
  value += ':';
  value += view.bleVoiceStreamTimeouts;
  value += ':';
  value += view.wirelessHolding;
  value += ':';
  value += view.message;
  value += ':';
  value += browserState_.selectionMode();
  value += ':';
  value += browserState_.selectedCount();
  value += ':';
  value += view.undoAvailable;
  value += ':';
  value += state_.capsuleScopeOverlay;
  value += ':';
  value += state_.detailMoreOverlay;
  value += ':';
  value += state_.purgeConfirmOverlay;
  value += ':';
  value += purgeConfirmCount_;
  if (state_.screen() == UiScreen::capsuleDetail ||
      state_.screen() == UiScreen::capsules) {
    value += ':';
    value += view.library == nullptr ? 0 : view.library->count();
    if (includeScroll && view.library != nullptr &&
        state_.screen() == UiScreen::capsules) {
      const size_t firstIndex = static_cast<size_t>(
          capsuleScroll_.positionPx() / ui::kCapsuleRowStride);
      for (uint8_t row = 0; row < ui::kCapsuleVisibleRows + 2; ++row) {
        const CapsuleSummary *record = view.library->at(firstIndex + row);
        if (record == nullptr) break;
        value += ':';
        value += record->id;
        value += ':';
        value += static_cast<int>(record->status);
        value += ':';
        value += record->preview;
        value += ':';
        value += record->favorite;
        value += ':';
        value += record->readOnly;
        value += ':';
        value += record->durationMs;
        value += ':';
        value += browserState_.selected(record->id.c_str());
      }
    } else if (view.library != nullptr) {
      const CapsuleSummary *record = selected(*view.library);
      if (record != nullptr) {
        value += ':';
        value += static_cast<int>(record->status);
        value += ':';
        value += record->preview;
        value += ':';
        value += record->favorite;
        value += ':';
        value += record->readOnly;
      }
    }
  }
  if (state_.screen() == UiScreen::device ||
      state_.screen() == UiScreen::computerSync ||
      state_.screen() == UiScreen::bluetoothPairing ||
      state_.screen() == UiScreen::provisioning ||
      state_.screen() == UiScreen::provisioningLog) {
    value += ':';
    value += static_cast<int>(view.wifiPhase);
    value += ':';
    value += view.wifiSyncOpen;
    value += ':';
    value += view.wifiSyncSecureReady;
    value += ':';
    value += view.wifiSyncPaired;
    value += ':';
    value += view.wifiSyncNetworkConnected;
    value += ':';
    value += view.wifiSyncListener;
    value += ':';
    value += view.wifiSyncBonjour;
    value += ':';
    value += view.wifiSyncClient;
    value += ':';
    value += view.wifiSyncAuthenticated;
    value += ':';
    value += view.wifiSyncBusy;
    value += ':';
    value += view.wifiSyncCompleted;
    value += ':';
    value += view.wifiSyncRemainingSeconds;
    value += ':';
    value += view.wifiSyncLastCompletedAtMs;
    value += ':';
    value += view.wifiSyncLastError;
    value += ':';
    value += view.portalSsid;
    value += ':';
    value += view.portalPassword;
    value += ':';
    value += static_cast<int>(view.portalState);
    value += ':';
    value += view.portalStatus;
    value += ':';
    value += view.settings == nullptr ? false : view.settings->wifiEnabled;
    value += ':';
    value += view.settings == nullptr ? false : view.settings->raiseToWake;
    value += ':';
    value += view.board->ioExpander;
    value += view.board->display;
    value += view.board->touch;
    value += view.board->sdCard;
    value += view.board->rtc;
    value += view.board->imu;
    value += view.board->pmu;
    value += view.audioReady;
    value += view.usbReady;
    value += renderer_.sdFontReady();
    value += ':';
    value += view.provisioningDiagnostics == nullptr
        ? 0 : view.provisioningDiagnostics->revision();
    value += ':';
    value += includeScroll ? provisioningLogScroll_.positionPx() : 0;
  }
  return value;
}

String Dashboard::topBarSignature(const DashboardView &view) const {
  String value;
  value.reserve(64);
  value += view.board->batteryPercent;
  value += ':';
  value += view.board->charging;
  value += ':';
  value += view.board->sdCard;
  value += ':';
  value += view.usbConnected;
  value += ':';
  value += view.bleVoiceReady;
  value += ':';
  value += static_cast<int>(view.wifiPhase);
  value += ':';
  value += view.wifiSyncOpen;
  value += ':';
  value += view.wifiSyncSecureReady;
  value += ':';
  value += view.wifiSyncPaired;
  value += ':';
  value += view.wifiSyncNetworkConnected;
  value += ':';
  value += view.wifiSyncClient;
  value += ':';
  value += view.wifiSyncAuthenticated;
  value += ':';
  value += view.wifiSyncBusy;
  value += ':';
  value += view.wifiSyncCompleted;
  value += ':';
  value += view.wifiSyncRemainingSeconds;
  value += ':';
  value += view.wifiSyncLastError;
  return value;
}

UiAction Dashboard::actionAt(int16_t x, int16_t y,
                             bool voiceReady) const {
  return uiActionAt(state_, x, y, voiceReady);
}

void Dashboard::swipeHorizontal(int16_t deltaX, bool locked, int16_t startX) {
  if (state_.capsuleScopeOverlay || state_.detailMoreOverlay) return;
  if (locked || browserState_.rootSwipeLocked()) return;
  if (state_.screen() == UiScreen::capsuleDetail ||
      state_.screen() == UiScreen::computerSync ||
      state_.screen() == UiScreen::bluetoothPairing ||
      state_.screen() == UiScreen::provisioning ||
      state_.screen() == UiScreen::provisioningLog) {
    if (isBackEdgeSwipe(startX, deltaX)) back();
    return;
  }
  const RootPage next = swipedPage(state_.page, deltaX, false);
  if (next != state_.page) navigate(next);
}

ScrollPhysics *Dashboard::activeScroll() {
  if (state_.capsuleScopeOverlay || state_.detailMoreOverlay) return nullptr;
  switch (state_.screen()) {
    case UiScreen::capsules: return &capsuleScroll_;
    case UiScreen::capsuleDetail: return &detailScroll_;
    case UiScreen::provisioningLog: return &provisioningLogScroll_;
    default: return nullptr;
  }
}

const ScrollPhysics *Dashboard::activeScroll() const {
  if (state_.capsuleScopeOverlay || state_.detailMoreOverlay) return nullptr;
  switch (state_.screen()) {
    case UiScreen::capsules: return &capsuleScroll_;
    case UiScreen::capsuleDetail: return &detailScroll_;
    case UiScreen::provisioningLog: return &provisioningLogScroll_;
    default: return nullptr;
  }
}

int32_t Dashboard::capsuleScrollMaximum(
    const CapsuleLibrary &library) const {
  const int32_t viewportBottom = browserState_.selectionMode()
      ? ui::kCapsuleSelectionBarTop : ui::kCapsuleListBottom;
  const int32_t viewportHeight = viewportBottom - ui::kCapsuleListTop;
  const int32_t contentHeight = static_cast<int32_t>(library.count()) *
      ui::kCapsuleRowStride;
  return contentHeight > viewportHeight ? contentHeight - viewportHeight : 0;
}

int32_t Dashboard::provisioningScrollMaximum() const {
  const int32_t viewportHeight =
      ui::kProvisionLogListBottom - ui::kProvisionLogListTop;
  const int32_t contentHeight = static_cast<int32_t>(provisioningLogCount_) *
      ui::kProvisionLogRowStride;
  return contentHeight > viewportHeight ? contentHeight - viewportHeight : 0;
}

void Dashboard::updateScrollBounds(const CapsuleLibrary &library) {
  if (capsuleScroll_.setMaximum(capsuleScrollMaximum(library))) {
    invalidated_ = true;
  }
  if (provisioningLogScroll_.setMaximum(provisioningScrollMaximum())) {
    invalidated_ = true;
  }
}

bool Dashboard::beginVerticalScroll(int16_t y, uint32_t nowMs,
                                    const CapsuleLibrary &library) {
  updateScrollBounds(library);
  ScrollPhysics *scroll = activeScroll();
  return scroll != nullptr && scroll->beginDrag(y, nowMs);
}

bool Dashboard::updateVerticalScroll(int16_t y, uint32_t nowMs,
                                     const CapsuleLibrary &library) {
  updateScrollBounds(library);
  ScrollPhysics *scroll = activeScroll();
  if (scroll == nullptr) return false;
  const bool changed = scroll->dragTo(y, nowMs);
  if (changed) {
    invalidated_ = true;
    scrollFramePending_ = true;
  }
  return changed;
}

void Dashboard::endVerticalScroll(uint32_t nowMs) {
  ScrollPhysics *scroll = activeScroll();
  if (scroll != nullptr) scroll->endDrag(nowMs);
}

bool Dashboard::advanceVerticalScroll(uint32_t nowMs,
                                      const CapsuleLibrary &library) {
  updateScrollBounds(library);
  ScrollPhysics *scroll = activeScroll();
  if (scroll == nullptr) return false;
  const bool changed = scroll->tick(nowMs);
  if (changed) {
    invalidated_ = true;
    scrollFramePending_ = true;
  }
  return changed;
}

bool Dashboard::scrollActive() const {
  const ScrollPhysics *scroll = activeScroll();
  return scroll != nullptr && scroll->active();
}

bool Dashboard::openCapsuleAt(int16_t y,
                              const CapsuleLibrary &library) {
  const int16_t listBottom = browserState_.selectionMode()
      ? ui::kCapsuleSelectionBarTop : ui::kCapsuleListBottom;
  if (y < ui::kCapsuleListTop || y >= listBottom) return false;
  const int index = (capsuleScroll_.positionPx() + y -
      ui::kCapsuleListTop) / ui::kCapsuleRowStride;
  if (index < 0 || static_cast<size_t>(index) >= library.count()) return false;
  const CapsuleSummary *record = library.at(static_cast<size_t>(index));
  if (record == nullptr) return false;
  browserState_.focus(record->id.c_str());
  state_.capsuleDetail = true;
  state_.detailRetryEnabled = false;
  state_.detailTrashEnabled = false;
  detailScroll_.reset();
  detailBodyCache_ = "";
  detailBodyCacheKey_ = "";
  pageTransition_.prepare(PageTransitionDirection::fromRight);
  invalidated_ = true;
  return true;
}

bool Dashboard::beginCapsuleSelectionAt(
    int16_t y, const CapsuleLibrary &library) {
  if (y < ui::kCapsuleListTop || y >= ui::kCapsuleSelectionBarTop) {
    return false;
  }
  const size_t index = static_cast<size_t>((capsuleScroll_.positionPx() + y -
      ui::kCapsuleListTop) / ui::kCapsuleRowStride);
  const CapsuleSummary *record = library.at(index);
  if (record == nullptr || record->readOnly ||
      record->status == CapsuleStatus::transcribing) {
    return false;
  }
  const bool changed = browserState_.toggle(record->id.c_str(), true);
  state_.capsuleSelectionMode = browserState_.selectionMode();
  if (changed) invalidated_ = true;
  return changed;
}

bool Dashboard::toggleCapsuleSelectionAt(
    int16_t y, const CapsuleLibrary &library) {
  return beginCapsuleSelectionAt(y, library);
}

void Dashboard::clearCapsuleSelection() {
  browserState_.clearSelection();
  state_.capsuleSelectionMode = false;
  invalidated_ = true;
}

std::vector<String> Dashboard::selectedCapsuleIds(
    const CapsuleLibrary &library) const {
  std::vector<String> ids;
  ids.reserve(browserState_.selectedCount());
  for (const std::string &selectedId : browserState_.selectedIds()) {
    const CapsuleSummary *record = library.find(selectedId.c_str());
    if (record != nullptr) ids.push_back(record->id);
  }
  return ids;
}

void Dashboard::back() {
  if (ScrollPhysics *scroll = activeScroll()) scroll->cancelMotion();
  if (state_.provisioning) {
    if (state_.provisioningLog) {
      pageTransition_.prepare(PageTransitionDirection::fromLeft);
      state_.provisioningLog = false;
      provisioningLogScroll_.reset();
      invalidated_ = true;
      return;
    }
    pageTransition_.prepare(PageTransitionDirection::fromLeft);
    state_.provisioning = false;
    state_.capsuleScopeOverlay = false;
    state_.detailMoreOverlay = false;
    invalidated_ = true;
    return;
  }
  if (state_.bluetoothPairing) {
    pageTransition_.prepare(PageTransitionDirection::fromLeft);
    state_.bluetoothPairing = false;
    invalidated_ = true;
    return;
  }
  if (state_.computerSync) {
    pageTransition_.prepare(PageTransitionDirection::fromLeft);
    state_.computerSync = false;
    invalidated_ = true;
    return;
  }
  if (state_.capsuleScopeOverlay || state_.detailMoreOverlay ||
      state_.purgeConfirmOverlay) {
    closeOverlays();
    return;
  }
  if (browserState_.selectionMode()) {
    clearCapsuleSelection();
    return;
  }
  pageTransition_.prepare(PageTransitionDirection::fromLeft);
  state_.provisioning = false;
  state_.capsuleDetail = false;
  state_.detailRetryEnabled = false;
  state_.detailTrashEnabled = false;
  browserState_.clearFocus();
  detailScroll_.reset();
  detailBodyCache_ = "";
  detailBodyCacheKey_ = "";
  invalidated_ = true;
}

void Dashboard::openProvisioningLog() {
  if (state_.screen() != UiScreen::provisioning) return;
  provisioningLogScroll_.reset();
  pageTransition_.prepare(PageTransitionDirection::fromRight);
  state_.provisioningLog = true;
  invalidated_ = true;
}

void Dashboard::openBluetoothPairing() {
  if (state_.screen() != UiScreen::device) return;
  pageTransition_.prepare(PageTransitionDirection::fromRight);
  state_.bluetoothPairing = true;
  invalidated_ = true;
}

void Dashboard::openComputerSync() {
  const UiScreen screen = state_.screen();
  if (screen != UiScreen::home && screen != UiScreen::capsules &&
      screen != UiScreen::device) return;
  if (ScrollPhysics *scroll = activeScroll()) scroll->cancelMotion();
  pageTransition_.prepare(PageTransitionDirection::fromRight);
  state_.computerSync = true;
  state_.capsuleScopeOverlay = false;
  state_.detailMoreOverlay = false;
  state_.purgeConfirmOverlay = false;
  invalidated_ = true;
}

void Dashboard::scopeChanged() {
  closeOverlays();
  browserState_.clearSelection();
  state_.capsuleSelectionMode = false;
  capsuleScroll_.reset();
  browserState_.clearFocus();
  state_.capsuleDetail = false;
  state_.detailRetryEnabled = false;
  state_.detailTrashEnabled = false;
  detailScroll_.reset();
  detailBodyCache_ = "";
  detailBodyCacheKey_ = "";
  invalidated_ = true;
}

void Dashboard::openScopePicker() {
  if (state_.screen() != UiScreen::capsules ||
      browserState_.selectionMode()) return;
  capsuleScroll_.cancelMotion();
  state_.capsuleScopeOverlay = true;
  state_.detailMoreOverlay = false;
  state_.purgeConfirmOverlay = false;
  invalidated_ = true;
}

void Dashboard::openDetailMore() {
  if (state_.screen() != UiScreen::capsuleDetail) return;
  detailScroll_.cancelMotion();
  state_.detailMoreOverlay = true;
  state_.capsuleScopeOverlay = false;
  state_.purgeConfirmOverlay = false;
  invalidated_ = true;
}

void Dashboard::openPurgeConfirm(size_t count) {
  if (count == 0) return;
  if (state_.screen() != UiScreen::capsules &&
      state_.screen() != UiScreen::capsuleDetail) return;
  if (ScrollPhysics *scroll = activeScroll()) scroll->cancelMotion();
  purgeConfirmCount_ = count;
  state_.purgeConfirmOverlay = true;
  state_.capsuleScopeOverlay = false;
  state_.detailMoreOverlay = false;
  invalidated_ = true;
}

void Dashboard::closeOverlays() {
  if (!state_.capsuleScopeOverlay && !state_.detailMoreOverlay &&
      !state_.purgeConfirmOverlay) return;
  state_.capsuleScopeOverlay = false;
  state_.detailMoreOverlay = false;
  state_.purgeConfirmOverlay = false;
  purgeConfirmCount_ = 0;
  invalidated_ = true;
}

void Dashboard::navigate(RootPage page) {
  if (state_.capsuleDetail || state_.computerSync ||
      state_.bluetoothPairing ||
      state_.provisioning ||
      state_.capsuleScopeOverlay || state_.detailMoreOverlay ||
      state_.purgeConfirmOverlay ||
      state_.page == page) return;
  if (ScrollPhysics *scroll = activeScroll()) scroll->cancelMotion();
  pageTransition_.prepare(
      static_cast<uint8_t>(page) > static_cast<uint8_t>(state_.page)
          ? PageTransitionDirection::fromRight
          : PageTransitionDirection::fromLeft);
  state_.page = page;
  invalidated_ = true;
}

const CapsuleSummary *Dashboard::selected(
    const CapsuleLibrary &library) const {
  return browserState_.focusedId().empty() ? nullptr :
      library.find(browserState_.focusedId().c_str());
}

void Dashboard::reconcileCapsules(const CapsuleLibrary *library) {
  std::vector<std::string> visibleIds;
  std::vector<std::string> allRelevantIds;
  if (library != nullptr) {
    visibleIds.reserve(library->count());
    for (size_t index = 0; index < library->count(); ++index) {
      const CapsuleSummary *record = library->at(index);
      if (record != nullptr) {
        visibleIds.emplace_back(record->id.c_str());
      }
    }
    if (!browserState_.focusedId().empty() &&
        library->find(browserState_.focusedId().c_str()) != nullptr) {
      allRelevantIds.push_back(browserState_.focusedId());
    }
  }
  const size_t previousCount = browserState_.selectedCount();
  browserState_.retainKnown(visibleIds);
  browserState_.retainFocused(allRelevantIds);
  if (previousCount != browserState_.selectedCount()) invalidated_ = true;
  if (state_.capsuleDetail && browserState_.focusedId().empty()) {
    reconcileMissingCapsule(state_);
    detailScroll_.reset();
    detailBodyCache_ = "";
    detailBodyCacheKey_ = "";
    invalidated_ = true;
  }
  if (library == nullptr) {
    capsuleScroll_.reset();
  } else if (capsuleScroll_.setMaximum(capsuleScrollMaximum(*library))) {
    invalidated_ = true;
  }
}

}  // namespace pokepod
