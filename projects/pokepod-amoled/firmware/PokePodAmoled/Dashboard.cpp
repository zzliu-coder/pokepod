#include "Dashboard.h"

#include <new>

namespace pokepod {
namespace {

String wifiLabel(WifiPhase phase) {
  switch (phase) {
    case WifiPhase::online: return "已连接";
    case WifiPhase::connecting: return "连接中";
    case WifiPhase::grace: return "已连接";
    case WifiPhase::provisioning: return "配网中";
    case WifiPhase::error: return "需要检查";
    case WifiPhase::off: return "已关闭";
    case WifiPhase::disabled: return "未设置";
  }
  return "";
}

String capsuleStatus(CapsuleStatus status) {
  switch (status) {
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
  if (record.createdAt.length() >= 16) {
    return record.createdAt.substring(5, 10) + " " +
        record.createdAt.substring(11, 16);
  }
  return record.createdAt.isEmpty() ? String("刚刚") : record.createdAt;
}

String durationLabel(uint32_t durationMs) {
  if (durationMs == 0) return "";
  return String((durationMs + 500) / 1000) + " 秒";
}

bool generatedVoiceTitle(const String &title) {
  return title.startsWith("语音 20") && title.indexOf('T') >= 0;
}

String capsuleDisplayText(const CapsuleSummary &record) {
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
  if (view.recording && !lastRecording_) {
    smoothedPeak_ = 0;
    envelopeCeiling_ = 1200;
    animationTick_ = 0;
  }
  state_.provisioning = view.provisioning;
  if ((view.recording || view.dictationHolding) && !view.provisioning &&
      (state_.page != RootPage::home || state_.capsuleDetail)) {
    state_.page = RootPage::home;
    state_.capsuleDetail = false;
    state_.capsuleSelection = -1;
    invalidated_ = true;
  }
  state_.homeMode = view.recording ? HomeMode::recording :
      (view.transcribing ? HomeMode::transcribing : HomeMode::idle);
  const String currentSignature = signature(view);
  bool bodyRepainted = false;
  if (invalidated_) {
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
    ++fullRedrawCount_;
    lastSignature_ = currentSignature;
    lastTopBarSignature_ = topBarSignature(view);
    lastDictationHolding_ = view.dictationHolding;
    invalidated_ = false;
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
    lastTopBarSignature_ = topBarSignature(view);
    lastDictationHolding_ = view.dictationHolding;
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
    case UiScreen::provisioning: drawProvisioning(view); break;
    case UiScreen::home: drawHome(view); drawPageIndicator(); break;
    case UiScreen::capsules: drawCapsules(view); drawPageIndicator(); break;
    case UiScreen::device: drawDevice(view); drawPageIndicator(); break;
  }
  if (shouldDrawToast(!view.message.isEmpty(), view.recording,
                      view.dictationHolding,
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
  if (view.wifiPhase == WifiPhase::disabled ||
      view.wifiPhase == WifiPhase::off) {
    display_->drawLine(241, 11, 258, 28, ui::kMuted);
  }
  drawUiIcon(*display_, UiIcon::mac, 282, 8,
             view.hostConnected ? ui::kDictation : ui::kMuted);
  if (!board.sdCard) drawUiIcon(*display_, UiIcon::warning, 326, 8, ui::kError);
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
  if (view.transcribing) {
    drawCenteredText("正在转写", 80, UiTextSize::display, ui::kInk, true);
    drawCapsuleOrb(224, ui::kWaiting, ui::kSurfaceRaised, 108);
    return;
  }
  if (view.hostConnected) {
    drawHomeAction(ui::kHomePrimaryTop, ui::kHomePrimaryConnectedBottom,
                   false, false);
    drawHomeAction(ui::kHomeSecondaryTop, ui::kHomeSecondaryBottom,
                   true, view.dictationHolding);
    return;
  }
  drawCenteredText("语音胶囊", 76, UiTextSize::display, ui::kInk, true);
  drawCapsuleOrb(230, ui::kAccent, ui::kAccentDim, 118);
  drawCenteredText("轻触录音", 354, UiTextSize::body, ui::kInk, true);
}

void Dashboard::drawHomeAction(int16_t top, int16_t bottom, bool dictation,
                               bool holding) {
  const uint16_t accent = dictation ? ui::kDictation : ui::kAccent;
  const uint16_t dim = dictation ? ui::kDictationDim : ui::kAccentDim;
  const uint16_t fill = holding ? dim : ui::kSurface;
  display_->fillRoundRect(20, top, 328, bottom - top, 28, fill);
  display_->drawRoundRect(20, top, 328, bottom - top, 28,
                          holding ? accent : ui::kDivider);
  const int16_t centerY = top + (bottom - top) / 2;
  if (dictation) {
    display_->drawCircle(78, centerY, 29, dim);
    display_->drawCircle(78, centerY, 20, accent);
    display_->drawFastVLine(78, centerY - 12, 24, accent);
    display_->drawFastHLine(68, centerY, 20, accent);
  } else {
    drawCapsuleMark(*display_, 78, centerY, 82, 42, accent, fill);
  }
  renderer_.drawText(dictation ? "微信语音输入" : "语音胶囊",
                     132, centerY - 34, 196, 1,
                     ui::kInk, fill, 0, false, UiTextSize::display, true);
  renderer_.drawText(dictation ? (holding ? "松开结束" : "按住说话") :
                                  "轻触录音",
                     132, centerY + 10, 190, 1,
                     accent, fill, 0, false, UiTextSize::body, true);
}

void Dashboard::drawCapsules(const DashboardView &view) {
  const size_t count = view.library == nullptr ? 0 : view.library->count();
  renderer_.drawText("胶囊", 20, 58, 150, 1, ui::kInk, ui::kBackground,
                     0, false, UiTextSize::display, true);
  const String countText = String(count) + " 条";
  const int16_t countWidth = renderer_.measureTextWidth(countText);
  renderer_.drawText(countText, 348 - countWidth, 66, countWidth, 1,
                     ui::kMuted, ui::kBackground);
  if (count == 0) {
    drawCapsuleOrb(230, ui::kDisabled, ui::kSurfaceRaised, 88);
    drawCenteredText("暂无胶囊", 326, UiTextSize::body, ui::kMuted, true);
    return;
  }
  for (uint8_t row = 0; row < ui::kCapsuleVisibleRows; ++row) {
    const size_t index = listOffset_ + row;
    const CapsuleSummary *record = view.library->at(index);
    if (record == nullptr) break;
    const int16_t y = ui::kCapsuleListTop + row * ui::kCapsuleRowStride;
    const uint16_t stateColor = capsuleStatusColor(record->status);
    display_->fillCircle(25, y + 13, 3, stateColor);
    const String preview = capsuleDisplayText(*record);
    renderer_.drawText(preview, 42, y, 270, 1, ui::kInk, ui::kBackground,
                       0, true, UiTextSize::body, false);
    String metadata = capsuleTime(*record) + "  " +
        capsuleStatus(record->status);
    const String duration = durationLabel(record->durationMs);
    if (!duration.isEmpty()) metadata += "  " + duration;
    renderer_.drawText(metadata, 42, y + 34, 276, 1,
                       stateColor, ui::kBackground);
    if (record->favorite) drawUiIcon(*display_, UiIcon::star, 322, y, ui::kWaiting);
    display_->drawFastHLine(42, y + 67, 306, ui::kDivider);
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
  const String status = capsuleStatus(record->status);
  const int16_t statusWidth = renderer_.measureTextWidth(status);
  renderer_.drawText(status, 348 - statusWidth, 20, statusWidth, 1,
                     capsuleStatusColor(record->status), ui::kBackground);
  renderer_.drawText(capsuleTime(*record), 20, 58, 180, 1,
                     ui::kMuted, ui::kBackground);
  String body = view.library->readBestText(*record);
  if (body == record->title && generatedVoiceTitle(record->title)) {
    body = capsuleDisplayText(*record);
  }
  renderer_.drawText(body, 20, 88, 328, 10, ui::kInk, ui::kBackground,
                     detailLineOffset_, true, UiTextSize::body);

  const bool needsRetry = record->status == CapsuleStatus::failed;
  state_.detailRetryEnabled = needsRetry;
  drawDetailAction(6, view.playing ? UiIcon::stop : UiIcon::play,
                   view.playing ? "停止" : "播放", true, ui::kAccent);
  drawDetailAction(98, UiIcon::star,
                   record->favorite ? "已收藏" : "收藏", false,
                   record->favorite ? ui::kWaiting : ui::kMuted);
  drawDetailAction(190, UiIcon::archive, "归档", false, ui::kMuted);
  drawDetailAction(282, UiIcon::retry, "重试", false,
                   needsRetry ? ui::kError : ui::kDisabled);
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
  const String healthText = health.ready() ? "状态正常" : "需要检查";
  const int16_t healthWidth = renderer_.measureTextWidth(healthText);
  renderer_.drawText(healthText, 348 - healthWidth, 66, healthWidth, 1,
                     health.ready() ? ui::kAccent : ui::kError,
                     ui::kBackground);

  const bool wifiEnabled = view.settings != nullptr &&
      view.settings->wifiEnabled;
  drawSettingRow(ui::kDeviceWifiTop, UiIcon::wifi, "无线网络",
                 wifiLabel(view.wifiPhase), wifiColor(view.wifiPhase),
                 SettingAccessory::toggle, wifiEnabled);
  drawSettingRow(ui::kDeviceMacTop, UiIcon::mac, "Mac 连接",
                 view.hostConnected ? "已连接" : "未连接",
                 view.hostConnected ? ui::kDictation : ui::kMuted,
                 SettingAccessory::value);
  drawSettingRow(ui::kDeviceStorageTop, UiIcon::storage, "存储与字库",
                 !view.board->sdCard ? "SD 需要检查" :
                 (renderer_.sdFontReady() ? "可用" : "字库未安装"),
                 view.board->sdCard && renderer_.sdFontReady()
                     ? ui::kAccent : ui::kError,
                 SettingAccessory::value);
  const bool raiseEnabled = view.settings != nullptr &&
      view.settings->raiseToWake;
  drawSettingRow(ui::kDeviceRaiseTop, UiIcon::raise, "抬起亮屏",
                 "", raiseEnabled ? ui::kAccent : ui::kMuted,
                 SettingAccessory::toggle, raiseEnabled);
  drawSettingRow(ui::kDeviceProvisionTop, UiIcon::phone, "手机配网",
                 "", ui::kMuted, SettingAccessory::chevron);
}

void Dashboard::drawProvisioning(const DashboardView &view) {
  drawBackButton();
  renderer_.drawText("连接手机", 64, 18, 220, 1, ui::kInk, ui::kBackground,
                     0, false, UiTextSize::body, true);
  renderer_.drawText("热点名称", 20, 92, 180, 1,
                     ui::kMuted, ui::kBackground);
  display_->fillRoundRect(20, 120, 328, 82, 20, ui::kSurface);
  renderer_.drawText(view.portalSsid, 38, 146, 292, 1,
                     ui::kInk, ui::kSurface, 0, false, UiTextSize::body, true);
  renderer_.drawText("密码", 20, 232, 100, 1,
                     ui::kMuted, ui::kBackground);
  display_->fillRoundRect(20, 260, 328, 82, 20, ui::kSurface);
  renderer_.drawText(view.portalPassword, 38, 286, 292, 1,
                     ui::kWaiting, ui::kSurface, 0, false,
                     UiTextSize::display, true);
  drawCenteredText("手机连接热点后会打开配置页", 382,
                   UiTextSize::compact, ui::kMuted);
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
      message.indexOf("尚未") >= 0 || message.indexOf("请插入") >= 0;
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
  if (screen == UiScreen::capsuleDetail || screen == UiScreen::provisioning) {
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
  if (screen == UiScreen::home && state_.homeMode == HomeMode::idle &&
      view.hostConnected && view.dictationHolding != lastDictationHolding_) {
    drawHomeAction(ui::kHomeSecondaryTop, ui::kHomeSecondaryBottom,
                   true, view.dictationHolding);
    presentRegion(20, ui::kHomeSecondaryTop, 328,
                  ui::kHomeSecondaryBottom - ui::kHomeSecondaryTop);
    lastDictationHolding_ = view.dictationHolding;
    ++partialRedrawCount_;
  }
}

void Dashboard::presentFrame() {
  if (frame_ != nullptr) frame_->flush();
}

void Dashboard::presentRegion(int16_t x, int16_t y,
                              int16_t width, int16_t height) {
  if (frame_ == nullptr || output_ == nullptr) return;
  uint8_t *pixels = frame_->getFramebuffer() +
      static_cast<int32_t>(y) * ui::kScreenWidth + x;
  output_->drawIndexedBitmap(x, y, pixels, frame_->getColorIndex(),
                             width, height, ui::kScreenWidth - width);
}

String Dashboard::signature(const DashboardView &view) const {
  String value;
  value.reserve(320);
  value += static_cast<int>(state_.screen());
  value += ':';
  value += static_cast<int>(state_.page);
  value += ':';
  value += state_.capsuleSelection;
  value += ':';
  value += listOffset_;
  value += ':';
  value += detailLineOffset_;
  value += ':';
  value += view.recording;
  value += ':';
  value += view.transcribing;
  value += ':';
  value += view.playing;
  value += ':';
  value += view.hostConnected;
  value += ':';
  value += view.message;
  if (state_.screen() == UiScreen::capsuleDetail ||
      state_.screen() == UiScreen::capsules) {
    value += ':';
    value += view.library == nullptr ? 0 : view.library->count();
    if (view.library != nullptr && state_.screen() == UiScreen::capsules) {
      for (uint8_t row = 0; row < ui::kCapsuleVisibleRows; ++row) {
        const CapsuleSummary *record = view.library->at(listOffset_ + row);
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
        value += record->durationMs;
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
      }
    }
  }
  if (state_.screen() == UiScreen::device ||
      state_.screen() == UiScreen::provisioning) {
    value += ':';
    value += static_cast<int>(view.wifiPhase);
    value += ':';
    value += view.portalSsid;
    value += ':';
    value += view.portalPassword;
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
  value += view.hostConnected;
  value += ':';
  value += static_cast<int>(view.wifiPhase);
  return value;
}

UiAction Dashboard::actionAt(int16_t x, int16_t y,
                             bool hostConnected) const {
  return uiActionAt(state_, x, y, hostConnected);
}

void Dashboard::swipeHorizontal(int16_t deltaX, bool locked, int16_t startX) {
  if (locked) return;
  if (state_.screen() == UiScreen::capsuleDetail ||
      state_.screen() == UiScreen::provisioning) {
    if (isBackEdgeSwipe(startX, deltaX)) back();
    return;
  }
  const RootPage next = swipedPage(state_.page, deltaX, false);
  if (next != state_.page) navigate(next);
}

void Dashboard::swipeVertical(int16_t deltaY,
                              const CapsuleLibrary &library) {
  bool changed = false;
  if (state_.screen() == UiScreen::capsuleDetail) {
    const uint16_t next = scrolledOffset(detailLineOffset_, deltaY, true, 4);
    changed = next != detailLineOffset_;
    detailLineOffset_ = next;
  } else if (state_.screen() == UiScreen::capsules) {
    const bool canIncrease =
        listOffset_ + ui::kCapsuleVisibleRows < library.count();
    const uint16_t next = scrolledOffset(listOffset_, deltaY, canIncrease, 1);
    changed = next != listOffset_;
    listOffset_ = static_cast<uint8_t>(next);
  }
  if (changed) invalidated_ = true;
}

bool Dashboard::openCapsuleAt(int16_t y,
                              const CapsuleLibrary &library) {
  if (y < ui::kCapsuleListTop || y >= ui::kCapsuleListBottom) return false;
  const int index = listOffset_ +
      (y - ui::kCapsuleListTop) / ui::kCapsuleRowStride;
  if (index < 0 || static_cast<size_t>(index) >= library.count()) return false;
  state_.capsuleSelection = index;
  state_.capsuleDetail = true;
  state_.detailRetryEnabled = false;
  detailLineOffset_ = 0;
  invalidated_ = true;
  return true;
}

void Dashboard::back() {
  state_.provisioning = false;
  state_.capsuleDetail = false;
  state_.detailRetryEnabled = false;
  state_.capsuleSelection = -1;
  detailLineOffset_ = 0;
  invalidated_ = true;
}

void Dashboard::navigate(RootPage page) {
  if (state_.capsuleDetail || state_.provisioning || state_.page == page) return;
  state_.page = page;
  invalidated_ = true;
}

const CapsuleSummary *Dashboard::selected(
    const CapsuleLibrary &library) const {
  return state_.capsuleSelection < 0 ? nullptr :
      library.at(static_cast<size_t>(state_.capsuleSelection));
}

}  // namespace pokepod
