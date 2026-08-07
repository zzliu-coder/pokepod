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
    case CapsuleStatus::damaged:
      return ui::kError;
    case CapsuleStatus::queued:
    case CapsuleStatus::transcribing:
    case CapsuleStatus::correcting:
      return ui::kWaiting;
    case CapsuleStatus::recording:
    case CapsuleStatus::rawReady:
    case CapsuleStatus::ready:
      return ui::kAccent;
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

uint16_t wifiColor(WifiPhase phase) {
  switch (phase) {
    case WifiPhase::online:
    case WifiPhase::grace:
      return ui::kAccent;
    case WifiPhase::connecting:
    case WifiPhase::provisioning:
      return ui::kWaiting;
    case WifiPhase::error:
      return ui::kError;
    case WifiPhase::off:
    case WifiPhase::disabled:
      return ui::kMuted;
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
    recordingCanvas_ = new (std::nothrow) Arduino_Canvas_Indexed(
        328, 188, output_, 20, 106);
    if (recordingCanvas_ == nullptr ||
        !recordingCanvas_->begin(GFX_SKIP_OUTPUT_BEGIN)) {
      delete recordingCanvas_;
      recordingCanvas_ = nullptr;
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
  if ((view.recording || view.dictationHolding) &&
      (state_.page != RootPage::home || state_.capsuleDetail)) {
    state_.page = RootPage::home;
    state_.capsuleDetail = false;
    state_.capsuleSelection = -1;
    invalidated_ = true;
  }
  state_.homeMode = view.recording ? HomeMode::recording :
      (view.transcribing ? HomeMode::transcribing : HomeMode::idle);
  const String currentSignature = signature(view);
  if (invalidated_) {
    display_->fillScreen(ui::kBackground);
    if (!state_.capsuleDetail) drawTopBar(view);
    drawBody(view);
    presentFrame();
    ++fullRedrawCount_;
    lastSignature_ = currentSignature;
    lastTopBarSignature_ = topBarSignature(view);
    lastDictationHolding_ = view.dictationHolding;
    lastRecordingSecond_ = UINT32_MAX;
    smoothedPeak_ = 0;
    animationTick_ = 0;
    invalidated_ = false;
  } else if (currentSignature != lastSignature_) {
    if (state_.capsuleDetail) {
      display_->fillScreen(ui::kBackground);
    } else {
      display_->fillRect(0, ui::kTopBarHeight, ui::kScreenWidth,
                         ui::kScreenHeight - ui::kTopBarHeight,
                         ui::kBackground);
    }
    drawBody(view);
    presentFrame();
    ++bodyRedrawCount_;
    lastSignature_ = currentSignature;
    lastDictationHolding_ = view.dictationHolding;
    lastRecordingSecond_ = UINT32_MAX;
  }
  drawDynamicRegions(view);
  if (!state_.capsuleDetail && state_.page == RootPage::home &&
      view.recording) {
    drawRecordingDynamic(view);
  }
}

void Dashboard::drawBody(const DashboardView &view) {
  if (state_.capsuleDetail) {
    drawCapsuleDetail(view);
    return;
  }
  if (state_.page == RootPage::home) drawHome(view);
  else if (state_.page == RootPage::capsules) drawCapsules(view);
  else drawDevice(view);
  if (!(state_.page == RootPage::home && view.recording)) drawBottomNav();
  if (!view.message.isEmpty() &&
      !(state_.page == RootPage::home && view.dictationHolding)) {
    drawToast(view.message);
  }
}

void Dashboard::drawTopBar(const DashboardView &view) {
  const BoardStatus &board = *view.board;
  const uint16_t batteryColor = board.charging ? ui::kAccent : ui::kInk;
  display_->drawRoundRect(24, 15, 26, 13, 2, batteryColor);
  display_->fillRect(50, 19, 3, 5, batteryColor);
  if (board.batteryPercent >= 0) {
    const int16_t fill = static_cast<int16_t>(
        board.batteryPercent > 100 ? 22 : board.batteryPercent * 22 / 100);
    if (fill > 0) display_->fillRect(26, 17, fill, 9, batteryColor);
  }
  if (board.charging) {
    display_->drawLine(36, 16, 32, 22, ui::kBackground);
    display_->drawLine(32, 22, 37, 22, ui::kBackground);
    display_->drawLine(37, 22, 34, 27, ui::kBackground);
  }
  display_->setTextSize(2);
  display_->setTextColor(ui::kInk, ui::kBackground);
  display_->setCursor(62, 14);
  if (board.batteryPercent >= 0) {
    display_->print(board.batteryPercent);
    display_->print('%');
  } else {
    display_->print("--");
  }
  drawUiIcon(*display_, UiIcon::wifi, 238, 8, wifiColor(view.wifiPhase));
  if (view.wifiPhase == WifiPhase::disabled ||
      view.wifiPhase == WifiPhase::off) {
    display_->drawLine(241, 11, 258, 28, ui::kMuted);
  }
  drawUiIcon(*display_, UiIcon::mac, 282, 8,
             view.hostConnected ? ui::kDictation : ui::kMuted);
  if (!board.sdCard) {
    drawUiIcon(*display_, UiIcon::warning, 326, 8, ui::kError);
  }
  display_->drawFastHLine(24, 42, 320, ui::kDivider);
}

void Dashboard::drawHome(const DashboardView &view) {
  if (view.recording) {
    renderer_.drawText("录音会先安全保存", 28, 58, 312, 1,
                       ui::kMuted, ui::kBackground);
    renderer_.drawText("正在记录", 28, 80, 220, 1,
                       ui::kInk, ui::kBackground, 0, false,
                       UiTextSize::body, true);
    drawCenteredText("轻触停止", 304, UiTextSize::body, ui::kInk, true);
    drawCenteredText("横向滑动已锁定", 338, UiTextSize::compact,
                     ui::kMuted);
    return;
  }
  if (view.transcribing) {
    renderer_.drawText("声音已经安全保存", 28, 58, 312, 1,
                       ui::kMuted, ui::kBackground);
    renderer_.drawText("正在转写", 28, 82, 220, 1,
                       ui::kInk, ui::kBackground, 0, false,
                       UiTextSize::display, true);
    drawCapsuleOrb(218, ui::kWaiting, ui::kSurfaceRaised);
    drawCenteredText("完成后会出现在胶囊列表", 310,
                     UiTextSize::compact, ui::kMuted);
    for (uint8_t index = 0; index < 3; ++index) {
      display_->fillCircle(174 + index * 10, 344, 2,
                           index == animationTick_ % 3
                               ? ui::kWaiting : ui::kDisabled);
    }
    return;
  }

  renderer_.drawText("随手说一句", 28, 58, 300, 1,
                     ui::kMuted, ui::kBackground);
  renderer_.drawText("语音胶囊", 28, 82, 260, 1,
                     ui::kInk, ui::kBackground, 0, false,
                     UiTextSize::display, true);
  drawCapsuleOrb(216, ui::kAccent, ui::kAccentDim);
  drawCenteredText("轻触开始录音", 298, UiTextSize::body,
                   ui::kInk, true);
  drawCenteredText("最长 58 秒", 324, UiTextSize::compact, ui::kMuted);
  if (view.hostConnected) drawDictationRail(view);
}

void Dashboard::drawCapsules(const DashboardView &view) {
  const size_t count = view.library == nullptr ? 0 : view.library->count();
  renderer_.drawText(String(count) + " 条胶囊", 28, 56, 250, 1,
                     ui::kMuted, ui::kBackground);
  renderer_.drawText("胶囊", 28, 78, 180, 1,
                     ui::kInk, ui::kBackground, 0, false,
                     UiTextSize::display, true);
  if (count == 0) {
    drawCapsuleMark(*display_, 184, 218, 96, 48,
                    ui::kDisabled, ui::kBackground);
    drawCenteredText("还没有胶囊", 270, UiTextSize::body,
                     ui::kInk, true);
    drawCenteredText("回到首页说下第一条", 304,
                     UiTextSize::compact, ui::kMuted);
    return;
  }
  for (uint8_t row = 0; row < ui::kCapsuleVisibleRows; ++row) {
    const size_t index = listOffset_ + row;
    const CapsuleSummary *record = view.library->at(index);
    if (record == nullptr) break;
    const int16_t y = ui::kCapsuleListTop + row * ui::kCapsuleRowStride;
    const uint16_t stateColor = capsuleStatusColor(record->status);
    display_->fillCircle(29, y + 14, 3, stateColor);
    const String preview =
        record->preview.isEmpty() ? record->title : record->preview;
    renderer_.drawText(preview, 46, y + 2, 268, 1,
                       ui::kInk, ui::kBackground, 0, true,
                       UiTextSize::body, false);
    String metadata = capsuleTime(*record) + "  " +
        capsuleStatus(record->status);
    const String duration = durationLabel(record->durationMs);
    if (!duration.isEmpty()) metadata += "  " + duration;
    renderer_.drawText(metadata, 46, y + 34, 270, 1,
                       stateColor, ui::kBackground);
    if (record->favorite) {
      drawUiIcon(*display_, UiIcon::star, 322, y + 4, ui::kWaiting);
    }
    display_->drawFastHLine(46, y + 63, 298, ui::kDivider);
  }
}

void Dashboard::drawCapsuleDetail(const DashboardView &view) {
  drawUiIcon(*display_, UiIcon::back, 18, 12, ui::kAccent);
  renderer_.drawText("返回", 48, 16, 90, 1,
                     ui::kInk, ui::kBackground);
  const CapsuleSummary *record =
      view.library == nullptr ? nullptr : selected(*view.library);
  if (record == nullptr) {
    drawCenteredText("胶囊需要检查", 178, UiTextSize::body,
                     ui::kError, true);
    return;
  }
  const String status = capsuleStatus(record->status);
  const int16_t statusWidth = renderer_.measureTextWidth(
      status, UiTextSize::compact);
  renderer_.drawText(status, 342 - statusWidth, 16, statusWidth, 1,
                     capsuleStatusColor(record->status), ui::kBackground);
  display_->drawFastHLine(24, 48, 320, ui::kDivider);
  const String body = view.library->readBestText(*record);
  renderer_.drawText(body, 24, 68, 320, 8,
                     ui::kInk, ui::kBackground,
                     detailLineOffset_, true, UiTextSize::body);

  drawDetailAction(18, view.playing ? UiIcon::stop : UiIcon::play,
                   view.playing ? "停止" : "播放", true, ui::kAccent);
  drawDetailAction(98, UiIcon::star,
                   record->favorite ? "已收藏" : "收藏",
                   false, record->favorite ? ui::kWaiting : ui::kMuted);
  drawDetailAction(178, UiIcon::archive, "归档", false, ui::kMuted);
  const bool needsRetry = record->status == CapsuleStatus::failed;
  state_.detailRetryEnabled = needsRetry;
  drawDetailAction(258, UiIcon::retry, "重试", false,
                   needsRetry ? ui::kError : ui::kDisabled);
}

void Dashboard::drawDevice(const DashboardView &view) {
  if (view.provisioning) {
    renderer_.drawText("五分钟临时热点", 28, 56, 280, 1,
                       ui::kMuted, ui::kBackground);
    renderer_.drawText("连接手机", 28, 78, 220, 1,
                       ui::kInk, ui::kBackground, 0, false,
                       UiTextSize::display, true);
    renderer_.drawText("在手机无线网络中选择", 28, 124, 312, 1,
                       ui::kMuted, ui::kBackground);
    display_->fillRoundRect(24, 154, 320, 72, 16, ui::kSurface);
    renderer_.drawText("热点名称", 42, 166, 260, 1,
                       ui::kMuted, ui::kSurface);
    renderer_.drawText(view.portalSsid, 42, 190, 280, 1,
                       ui::kInk, ui::kSurface, 0, false,
                       UiTextSize::body, true);
    display_->fillRoundRect(24, 242, 320, 72, 16, ui::kSurface);
    renderer_.drawText("密码", 42, 254, 260, 1,
                       ui::kMuted, ui::kSurface);
    renderer_.drawText(view.portalPassword, 42, 278, 280, 1,
                       ui::kWaiting, ui::kSurface, 0, false,
                       UiTextSize::body, true);
    drawCenteredText("保存成功后热点会自动关闭", 340,
                     UiTextSize::compact, ui::kMuted);
    return;
  }

  String deviceMeta = String(variantName(view.board->variant)) + "  设备详情";
  renderer_.drawText(deviceMeta, 28, 56, 300, 1,
                     ui::kMuted, ui::kBackground);
  renderer_.drawText("设备", 28, 78, 180, 1,
                     ui::kInk, ui::kBackground, 0, false,
                     UiTextSize::display, true);
  DeviceHealthState deviceHealth;
  deviceHealth.ioExpander = view.board->ioExpander;
  deviceHealth.display = view.board->display;
  deviceHealth.touch = view.board->touch;
  deviceHealth.sdCard = view.board->sdCard;
  deviceHealth.rtc = view.board->rtc;
  deviceHealth.imu = view.board->imu;
  deviceHealth.pmu = view.board->pmu;
  deviceHealth.audio = view.audioReady;
  deviceHealth.usb = view.usbReady;
  deviceHealth.fullTextFont = renderer_.sdFontReady();
  const bool healthy = deviceHealth.ready();
  const String health = healthy ? "状态正常" : "需要检查";
  const int16_t healthWidth = renderer_.measureTextWidth(health);
  renderer_.drawText(health, 340 - healthWidth, 86, healthWidth, 1,
                     healthy ? ui::kAccent : ui::kError, ui::kBackground);

  drawSettingRow(ui::kDeviceWifiTop, UiIcon::wifi, "无线网络",
                 wifiLabel(view.wifiPhase), wifiColor(view.wifiPhase));
  const bool wifiEnabled =
      view.settings != nullptr && view.settings->wifiEnabled;
  drawToggle(*display_, 300, ui::kDeviceWifiTop + 16, wifiEnabled,
             ui::kAccent, ui::kDisabled, ui::kBackground);

  drawSettingRow(ui::kDeviceMacTop, UiIcon::mac, "Mac 连接",
                 view.hostConnected ? "已连接" : "未连接",
                 view.hostConnected ? ui::kDictation : ui::kMuted);
  drawSettingRow(ui::kDeviceStorageTop, UiIcon::storage, "存储与字库",
                 !view.board->sdCard ? "SD 需要检查" :
                 (renderer_.sdFontReady() ? "可用" : "字库未安装"),
                 view.board->sdCard && renderer_.sdFontReady()
                     ? ui::kAccent : ui::kError);
  drawSettingRow(ui::kDeviceRaiseTop, UiIcon::raise, "抬起亮屏",
                 "抬起设备时亮屏", ui::kMuted);
  const bool raiseEnabled =
      view.settings != nullptr && view.settings->raiseToWake;
  drawToggle(*display_, 300, ui::kDeviceRaiseTop + 16, raiseEnabled,
             ui::kAccent, ui::kDisabled, ui::kBackground);
  drawSettingRow(ui::kDeviceProvisionTop, UiIcon::phone, "手机配网",
                 "扫描附近网络", ui::kMuted);
  drawUiIcon(*display_, UiIcon::chevron, 324,
             ui::kDeviceProvisionTop + 16, ui::kMuted);
}

void Dashboard::drawBottomNav() {
  display_->drawFastHLine(24, ui::kBottomNavTop, 320, ui::kDivider);
  const RootPage pages[] = {
      RootPage::home, RootPage::capsules, RootPage::device};
  const UiIcon icons[] = {
      UiIcon::capsule, UiIcon::list, UiIcon::device};
  const int16_t centers[] = {61, 184, 307};
  for (uint8_t index = 0; index < 3; ++index) {
    const bool selectedPage = state_.page == pages[index];
    const uint16_t color = selectedPage ? ui::kAccent : ui::kDisabled;
    drawUiIcon(*display_, icons[index], centers[index] - 12, 410, color);
    if (selectedPage) display_->fillCircle(centers[index], 442, 2, color);
  }
}

void Dashboard::drawCapsuleOrb(int16_t centerY, uint16_t accent,
                               uint16_t dimAccent) {
  display_->drawEllipse(184, centerY, 108, 76, ui::kSurfaceRaised);
  display_->drawEllipse(184, centerY, 92, 64, dimAccent);
  display_->drawEllipse(184, centerY, 91, 63, ui::kDivider);
  drawCapsuleMark(*display_, 184, centerY, 112, 56,
                  accent, ui::kSurface);
}

void Dashboard::drawDictationRail(const DashboardView &view) {
  const bool holding = view.dictationHolding;
  const uint16_t fill = holding ? ui::kDictationDim : ui::kSurface;
  display_->fillRoundRect(24, ui::kDictationTop, 320,
                          ui::kDictationBottom - ui::kDictationTop,
                          16, fill);
  if (holding) {
    display_->drawRoundRect(24, ui::kDictationTop, 320,
                            ui::kDictationBottom - ui::kDictationTop,
                            16, ui::kDictation);
  }
  display_->drawCircle(50, ui::kDictationTop + 25, 8, ui::kDictation);
  display_->drawFastHLine(46, ui::kDictationTop + 25, 8, ui::kDictation);
  display_->drawFastVLine(50, ui::kDictationTop + 21, 8, ui::kDictation);
  renderer_.drawText(holding ? "正在输入  松开结束" :
                                  "按住  微信语音输入",
                     72, ui::kDictationTop + 15, 252, 1,
                     ui::kInk, fill, 0, false,
                     UiTextSize::compact, true);
}

void Dashboard::drawToast(const String &message) {
  const bool warning = message.indexOf("失败") >= 0 ||
      message.indexOf("异常") >= 0 ||
      message.indexOf("检查") >= 0 ||
      message.indexOf("尚未") >= 0 ||
      message.indexOf("请插入") >= 0;
  const uint16_t statusColor = warning ? ui::kError : ui::kAccent;
  display_->fillRoundRect(24, 348, 320, 44, 14, ui::kSurfaceRaised);
  drawUiIcon(*display_, warning ? UiIcon::warning : UiIcon::check,
             34, 358, statusColor);
  renderer_.drawText(message, 66, 352, 260, 2,
                     ui::kInk, ui::kSurfaceRaised);
}

void Dashboard::drawCenteredText(const String &text, int16_t y,
                                 UiTextSize size, uint16_t color,
                                 bool bold, int16_t maxWidth) {
  const int16_t measured = renderer_.measureTextWidth(text, size);
  const int16_t width = measured < maxWidth ? measured : maxWidth;
  const int16_t x = (ui::kScreenWidth - width) / 2;
  renderer_.drawText(text, x, y, width, 1, color, ui::kBackground,
                     0, false, size, bold);
}

void Dashboard::drawSettingRow(int16_t top, UiIcon icon,
                               const String &title, const String &value,
                               uint16_t valueColor) {
  drawUiIcon(*display_, icon, 24, top + 16, valueColor);
  renderer_.drawText(title, 60, top + 4, 220, 1,
                     ui::kInk, ui::kBackground, 0, false,
                     UiTextSize::body, true);
  renderer_.drawText(value, 60, top + 34, 224, 1,
                     valueColor, ui::kBackground);
  display_->drawFastHLine(60, top + 55, 284, ui::kDivider);
}

void Dashboard::drawDetailAction(int16_t left, UiIcon icon,
                                 const String &label, bool emphasized,
                                 uint16_t accent) {
  const uint16_t fill = emphasized ? ui::kAccentDim : ui::kSurface;
  display_->fillRoundRect(left, ui::kDetailActionsTop, 72,
                          ui::kDetailActionsBottom -
                              ui::kDetailActionsTop,
                          14, fill);
  if (emphasized) {
    display_->drawRoundRect(left, ui::kDetailActionsTop, 72,
                            ui::kDetailActionsBottom -
                                ui::kDetailActionsTop,
                            14, accent);
  }
  drawUiIcon(*display_, icon, left + 24,
             ui::kDetailActionsTop + 7, accent);
  const int16_t measured =
      renderer_.measureTextWidth(label, UiTextSize::compact);
  renderer_.drawText(label, left + (72 - measured) / 2,
                     ui::kDetailActionsTop + 40, measured, 1,
                     emphasized ? ui::kInk : accent, fill);
}

void Dashboard::drawRecordingDynamic(const DashboardView &view) {
  Arduino_GFX *target = recordingCanvas_ != nullptr
      ? static_cast<Arduino_GFX *>(recordingCanvas_) : display_;
  const int16_t offsetX = recordingCanvas_ == nullptr ? 20 : 0;
  const int16_t offsetY = recordingCanvas_ == nullptr ? 106 : 0;
  target->fillRect(offsetX, offsetY, 328, 188, ui::kBackground);

  smoothedPeak_ = static_cast<uint16_t>(
      (static_cast<uint32_t>(smoothedPeak_) * 3 + view.audioPeak) / 4);
  const uint8_t phase = animationTick_ % 16;
  const uint8_t breath = phase <= 8 ? phase : 16 - phase;
  uint8_t energy = static_cast<uint8_t>(smoothedPeak_ / 1200);
  if (energy > 14) energy = 14;

  const int16_t centerX = offsetX + 164;
  const int16_t centerY = offsetY + 112;
  target->drawEllipse(centerX, centerY, 96 + breath + energy,
                      58 + breath / 2 + energy / 2, ui::kAccentDim);
  target->drawEllipse(centerX, centerY, 84 + energy,
                      50 + energy / 2, ui::kDivider);
  drawCapsuleMark(*target, centerX, centerY, 116, 58,
                  ui::kAccent, ui::kSurface);

  const int16_t waveCenter = centerY;
  for (int8_t index = -5; index <= 5; ++index) {
    const uint8_t distance = static_cast<uint8_t>(index < 0 ? -index : index);
    int16_t height = 4 + energy * (6 - distance) / 4;
    if (height > 38) height = 38;
    target->fillRoundRect(centerX + index * 8 - 2,
                          waveCenter - height / 2, 4, height, 2,
                          ui::kAccent);
  }

  const uint32_t second = view.recordingMs / 1000;
  char timer[16];
  snprintf(timer, sizeof(timer), "%02lu:%02lu",
           static_cast<unsigned long>(second / 60),
           static_cast<unsigned long>(second % 60));
  target->fillCircle(offsetX + 80, offsetY + 20, 3, ui::kError);
  target->setTextSize(2);
  target->setTextColor(ui::kMuted, ui::kBackground);
  target->setCursor(offsetX + 90, offsetY + 12);
  target->print("REC");
  target->setTextSize(4);
  target->setTextColor(ui::kInk, ui::kBackground);
  target->setCursor(offsetX + 104, offsetY + 2);
  target->print(timer);

  ++animationTick_;
  lastRecordingSecond_ = second;
  if (recordingCanvas_ != nullptr) recordingCanvas_->flush();
  else presentFrame();
}

void Dashboard::drawDynamicRegions(const DashboardView &view) {
  if (state_.capsuleDetail) return;
  const String currentTopBar = topBarSignature(view);
  if (currentTopBar != lastTopBarSignature_) {
    display_->fillRect(0, 0, ui::kScreenWidth,
                       ui::kTopBarHeight, ui::kBackground);
    drawTopBar(view);
    presentRegion(0, 0, ui::kScreenWidth, ui::kTopBarHeight);
    lastTopBarSignature_ = currentTopBar;
    ++partialRedrawCount_;
  }

  if (state_.page == RootPage::home &&
      state_.homeMode == HomeMode::idle &&
      view.hostConnected &&
      view.dictationHolding != lastDictationHolding_) {
    drawDictationRail(view);
    presentRegion(24, ui::kDictationTop, 320,
                  ui::kDictationBottom - ui::kDictationTop);
    lastDictationHolding_ = view.dictationHolding;
    ++partialRedrawCount_;
  }
  if (state_.page == RootPage::home &&
      state_.homeMode == HomeMode::transcribing) {
    display_->fillRect(160, 336, 48, 16, ui::kBackground);
    for (uint8_t index = 0; index < 3; ++index) {
      display_->fillCircle(174 + index * 10, 344, 2,
                           index == animationTick_ % 3
                               ? ui::kWaiting : ui::kDisabled);
    }
    presentRegion(160, 336, 48, 16);
    ++animationTick_;
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
  output_->drawIndexedBitmap(
      x, y, pixels, frame_->getColorIndex(), width, height,
      ui::kScreenWidth - width);
}

String Dashboard::signature(const DashboardView &view) const {
  String value;
  value.reserve(320);
  value += static_cast<int>(state_.page);
  value += ':';
  value += state_.capsuleDetail;
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
  if (state_.capsuleDetail || state_.page == RootPage::capsules) {
    value += ':';
    value += view.library == nullptr ? 0 : view.library->count();
  }
  if (state_.page == RootPage::device) {
    value += ':';
    value += static_cast<int>(view.wifiPhase);
    value += ':';
    value += view.provisioning;
    value += ':';
    value += view.portalSsid;
    value += ':';
    value += view.portalPassword;
    value += ':';
    value += view.settings == nullptr ? false : view.settings->wifiEnabled;
    value += ':';
    value += view.settings == nullptr ? false : view.settings->raiseToWake;
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

void Dashboard::swipeHorizontal(int16_t deltaX, bool locked) {
  if (state_.capsuleDetail) return;
  const RootPage next = swipedPage(state_.page, deltaX, locked);
  if (next != state_.page) navigate(next);
}

void Dashboard::swipeVertical(int16_t deltaY,
                              const CapsuleLibrary &library) {
  if (state_.capsuleDetail) {
    if (deltaY < -40) detailLineOffset_ += 4;
    else if (deltaY > 40 && detailLineOffset_ >= 4) detailLineOffset_ -= 4;
  } else if (state_.page == RootPage::capsules) {
    if (deltaY < -40 &&
        listOffset_ + ui::kCapsuleVisibleRows < library.count()) {
      ++listOffset_;
    } else if (deltaY > 40 && listOffset_ > 0) {
      --listOffset_;
    }
  }
  invalidated_ = true;
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
  state_.capsuleDetail = false;
  state_.detailRetryEnabled = false;
  state_.capsuleSelection = -1;
  detailLineOffset_ = 0;
  invalidated_ = true;
}

void Dashboard::navigate(RootPage page) {
  if (state_.capsuleDetail || state_.page == page) return;
  state_.page = page;
  invalidated_ = true;
}

const CapsuleSummary *Dashboard::selected(
    const CapsuleLibrary &library) const {
  return state_.capsuleSelection < 0 ? nullptr :
      library.at(static_cast<size_t>(state_.capsuleSelection));
}

}  // namespace pokepod
