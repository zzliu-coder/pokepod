#include "Dashboard.h"

namespace pokepod {
namespace {

constexpr uint16_t kPanel = 0x1082;
constexpr uint16_t kMuted = 0x7bef;
constexpr uint16_t kGreen = 0x0660;
constexpr uint16_t kBlue = 0x025f;
constexpr uint16_t kRed = 0xb104;
constexpr uint16_t kOrange = 0xfbe0;

String wifiLabel(WifiPhase phase) {
  switch (phase) {
    case WifiPhase::online: return "Wi-Fi 已连接";
    case WifiPhase::connecting: return "Wi-Fi 连接中";
    case WifiPhase::grace: return "Wi-Fi 已连接";
    case WifiPhase::provisioning: return "Wi-Fi 配网模式";
    case WifiPhase::error: return "Wi-Fi 异常";
    case WifiPhase::off: return "Wi-Fi 关闭";
    case WifiPhase::disabled: return "Wi-Fi 未设置";
  }
  return "Wi-Fi";
}

String capsuleStatus(CapsuleStatus status) {
  switch (status) {
    case CapsuleStatus::queued: return "待转写";
    case CapsuleStatus::transcribing: return "转写中";
    case CapsuleStatus::rawReady: return "转写完成";
    case CapsuleStatus::correcting: return "校对中";
    case CapsuleStatus::ready: return "完成";
    case CapsuleStatus::failed: return "失败";
    case CapsuleStatus::recording: return "录音中";
    case CapsuleStatus::damaged: return "异常";
  }
  return "";
}

}  // namespace

void Dashboard::begin(Arduino_GFX *display, fs::FS *fs) {
  display_ = display;
  renderer_.begin(display, fs);
  invalidated_ = true;
  if (display_ != nullptr) display_->fillScreen(RGB565_BLACK);
}

void Dashboard::draw(const DashboardView &view) {
  if (display_ == nullptr || view.board == nullptr) return;
  state_.homeMode = view.recording ? HomeMode::recording :
      (view.transcribing ? HomeMode::transcribing : HomeMode::idle);
  const String currentSignature = signature(view);
  if (invalidated_) {
    display_->fillScreen(RGB565_BLACK);
    if (!state_.capsuleDetail) drawTopBar(view);
    drawBody(view);
    ++fullRedrawCount_;
    lastSignature_ = currentSignature;
    lastTopBarSignature_ = topBarSignature(view);
    lastDictationHolding_ = view.dictationHolding;
    lastMessage_ = view.message;
    lastMessagePage_ = state_.page;
    lastRecordingSecond_ = UINT32_MAX;
    invalidated_ = false;
  } else if (currentSignature != lastSignature_) {
    // A state transition within the current page should not black out the
    // status bar or the whole AMOLED.  Clear only the page body and rebuild
    // that region; page navigation still uses the explicit full invalidation.
    display_->fillRect(0, 42, 368, 406, RGB565_BLACK);
    drawBody(view);
    ++bodyRedrawCount_;
    lastSignature_ = currentSignature;
    lastDictationHolding_ = view.dictationHolding;
    lastMessage_ = view.message;
    lastMessagePage_ = state_.page;
    lastRecordingSecond_ = UINT32_MAX;
  }
  drawDynamicRegions(view);
  if (!state_.capsuleDetail && state_.page == RootPage::home && view.recording) {
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
  drawPageDots();
}

void Dashboard::drawTopBar(const DashboardView &view) {
  const BoardStatus &board = *view.board;
  String text = "电池 ";
  text += board.batteryPercent >= 0 ? String(board.batteryPercent) + "%" : "--";
  if (board.charging) text += " 充电";
  text += view.wifiPhase == WifiPhase::online || view.wifiPhase == WifiPhase::grace
      ? " Wi-Fi" : "";
  if (view.hostConnected) text += " Mac";
  if (!board.sdCard) text += " SD 异常";
  renderer_.drawText(text, 14, 12, 340, 1, RGB565_WHITE, RGB565_BLACK);
  display_->drawFastHLine(12, 38, 344, kPanel);
}

void Dashboard::drawHome(const DashboardView &view) {
  renderer_.drawText("首页", 18, 58, 330, 1, RGB565_CYAN, RGB565_BLACK);
  if (view.recording) {
    renderer_.drawText("录音中", 132, 120, 160, 1, RGB565_WHITE, RGB565_BLACK);
    drawButton(24, 300, 320, 100, kRed, "停止", "最长 58.5 秒");
    return;
  }
  if (view.transcribing) {
    renderer_.drawText("腾讯云正在转写", 94, 150, 240, 1,
                       RGB565_WHITE, RGB565_BLACK);
    renderer_.drawText("屏幕和 Mac 连接仍可使用", 54, 210, 280, 2,
                       kMuted, RGB565_BLACK);
    return;
  }
  drawButton(24, 150, 320, 112, kGreen, "语音胶囊", "录音并自动转写");
  if (view.hostConnected) {
    drawButton(24, 286, 320, 100,
               view.dictationHolding ? kRed : kBlue,
               view.dictationHolding ? "正在说话" : "微信语音输入",
               view.dictationHolding ? "松开结束" : "按住说话，松开结束");
  } else {
    renderer_.drawText("连接 Mac 后显示语音输入", 52, 318, 280, 2,
                       kMuted, RGB565_BLACK);
  }
  if (!view.message.isEmpty()) {
    renderer_.drawText(view.message, 18, 410, 330, 1, RGB565_YELLOW, RGB565_BLACK);
  }
}

void Dashboard::drawCapsules(const DashboardView &view) {
  renderer_.drawText("胶囊列表", 18, 54, 300, 1, RGB565_CYAN, RGB565_BLACK);
  if (view.library == nullptr || view.library->count() == 0) {
    renderer_.drawText("暂无胶囊", 122, 205, 180, 1, kMuted, RGB565_BLACK);
    return;
  }
  for (uint8_t row = 0; row < 4; ++row) {
    const size_t index = listOffset_ + row;
    const CapsuleSummary *record = view.library->at(index);
    if (record == nullptr) break;
    const int16_t y = 78 + row * 82;
    display_->fillRoundRect(12, y, 344, 72, 10, kPanel);
    String title = record->favorite ? "★ " : "";
    title += record->preview;
    renderer_.drawText(title, 22, y + 10, 244, 2, RGB565_WHITE, kPanel, 0, true);
    renderer_.drawText(capsuleStatus(record->status), 272, y + 12, 76, 2,
                       record->status == CapsuleStatus::failed ? RGB565_RED : RGB565_CYAN,
                       kPanel);
  }
}

void Dashboard::drawCapsuleDetail(const DashboardView &view) {
  renderer_.drawText("‹ 返回", 14, 16, 150, 1, RGB565_CYAN, RGB565_BLACK);
  const CapsuleSummary *record = selected(*view.library);
  if (record == nullptr) {
    renderer_.drawText("胶囊异常", 120, 190, 180, 1, RGB565_RED, RGB565_BLACK);
    return;
  }
  renderer_.drawText(capsuleStatus(record->status), 246, 18, 108, 1,
                     RGB565_YELLOW, RGB565_BLACK);
  display_->drawFastHLine(12, 48, 344, kPanel);
  const String body = view.library->readBestText(*record);
  renderer_.drawText(body, 16, 68, 336, 14, RGB565_WHITE, RGB565_BLACK,
                     detailLineOffset_, true);
  drawButton(8, 362, 82, 66, kBlue, view.playing ? "停止" : "播放");
  drawButton(98, 362, 82, 66, kGreen, record->favorite ? "取消收藏" : "收藏");
  drawButton(188, 362, 82, 66, kOrange, "归档");
  drawButton(278, 362, 82, 66, kRed, "重试");
}

void Dashboard::drawDevice(const DashboardView &view) {
  renderer_.drawText("设置与设备详情", 18, 54, 330, 1, RGB565_CYAN, RGB565_BLACK);
  String details = String(variantName(view.board->variant)) + "\n" +
      wifiLabel(view.wifiPhase) + "  " + String(view.wifiRssi) + " dBm\n" +
      "SD " + (view.board->sdCard ? String("可用") : String("异常")) +
      "  字库 " + (renderer_.sdFontReady() ? String("可用") : String("未安装")) + "\n" +
      "麦克风 " + (view.audioReady ? String("可用") : String("异常")) +
      "  USB " + (view.hostConnected ? String("已连接") : String("未连接"));
  renderer_.drawText(details, 20, 86, 330, 4, RGB565_WHITE, RGB565_BLACK);
  const bool wifiEnabled = view.settings != nullptr && view.settings->wifiEnabled;
  drawButton(18, 178, 332, 62, wifiEnabled ? kGreen : kPanel,
             wifiEnabled ? "关闭无线网络" : "开启无线网络");
  drawButton(18, 252, 332, 76, kBlue, "手机配网", "五分钟临时热点");
  if (view.provisioning) {
    renderer_.drawText("热点 " + view.portalSsid + "\n密码 " + view.portalPassword,
                       22, 338, 330, 3, RGB565_YELLOW, RGB565_BLACK);
  } else {
    const bool raiseEnabled = view.settings != nullptr && view.settings->raiseToWake;
    drawButton(18, 338, 332, 62, raiseEnabled ? kGreen : kPanel,
               raiseEnabled ? "关闭抬起亮屏" : "开启抬起亮屏");
  }
  if (!view.provisioning && !view.message.isEmpty()) {
    renderer_.drawText(view.message, 22, 408, 324, 1,
                       RGB565_YELLOW, RGB565_BLACK);
  }
}

void Dashboard::drawPageDots() {
  if (state_.capsuleDetail) return;
  for (uint8_t index = 0; index < 3; ++index) {
    display_->fillCircle(172 + index * 12, 438, 3,
        static_cast<uint8_t>(state_.page) == index ? RGB565_WHITE : kMuted);
  }
}

void Dashboard::drawButton(int16_t x, int16_t y, int16_t width, int16_t height,
                           uint16_t color, const String &title,
                           const String &subtitle) {
  display_->fillRoundRect(x, y, width, height, 14, color);
  renderer_.drawText(title, x + 16, y + 14, width - 32, 2,
                     RGB565_WHITE, color);
  if (!subtitle.isEmpty()) {
    renderer_.drawText(subtitle, x + 16, y + height - 28, width - 32, 1,
                       RGB565_WHITE, color);
  }
}

void Dashboard::drawRecordingDynamic(const DashboardView &view) {
  const uint32_t second = view.recordingMs / 1000;
  if (second != lastRecordingSecond_) {
    display_->fillRect(118, 158, 150, 48, RGB565_BLACK);
    char timer[16];
    snprintf(timer, sizeof(timer), "%02lu:%02lu",
             static_cast<unsigned long>(second / 60),
             static_cast<unsigned long>(second % 60));
    display_->setTextSize(4);
    display_->setTextColor(RGB565_WHITE, RGB565_BLACK);
    display_->setCursor(120, 164);
    display_->print(timer);
    lastRecordingSecond_ = second;
  }
  display_->fillRect(28, 224, 312, 48, RGB565_BLACK);
  const uint8_t bars = 16;
  uint32_t activeValue = static_cast<uint32_t>(view.audioPeak) * bars / 18000;
  if (activeValue > bars) activeValue = bars;
  const uint8_t active = static_cast<uint8_t>(activeValue);
  for (uint8_t index = 0; index < bars; ++index) {
    const int16_t height = 8 + (index % 5) * 6;
    display_->fillRoundRect(30 + index * 19, 248 - height / 2, 12, height, 3,
                            index < active ? RGB565_GREEN : kPanel);
  }
}

void Dashboard::drawDynamicRegions(const DashboardView &view) {
  if (state_.capsuleDetail) return;
  const String currentTopBar = topBarSignature(view);
  if (currentTopBar != lastTopBarSignature_) {
    display_->fillRect(0, 0, 368, 41, RGB565_BLACK);
    drawTopBar(view);
    lastTopBarSignature_ = currentTopBar;
    ++partialRedrawCount_;
  }

  if (state_.page == RootPage::home && state_.homeMode == HomeMode::idle &&
      view.hostConnected && view.dictationHolding != lastDictationHolding_) {
    drawButton(24, 286, 320, 100,
               view.dictationHolding ? kRed : kBlue,
               view.dictationHolding ? "正在说话" : "微信语音输入",
               view.dictationHolding ? "松开结束" : "按住说话，松开结束");
    lastDictationHolding_ = view.dictationHolding;
    ++partialRedrawCount_;
  }

  if ((state_.page == RootPage::home || state_.page == RootPage::device) &&
      (view.message != lastMessage_ || state_.page != lastMessagePage_)) {
    display_->fillRect(12, 402, 344, 34, RGB565_BLACK);
    if (!view.message.isEmpty()) {
      renderer_.drawText(view.message,
                         state_.page == RootPage::home ? 18 : 22,
                         state_.page == RootPage::home ? 410 : 408,
                         state_.page == RootPage::home ? 330 : 324, 1,
                         RGB565_YELLOW, RGB565_BLACK);
    }
    lastMessage_ = view.message;
    lastMessagePage_ = state_.page;
    ++partialRedrawCount_;
  }
}

String Dashboard::signature(const DashboardView &view) const {
  String value;
  value.reserve(256);
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

UiAction Dashboard::actionAt(int16_t x, int16_t y, bool hostConnected) const {
  return uiActionAt(state_, x, y, hostConnected);
}

void Dashboard::swipeHorizontal(int16_t deltaX, bool locked) {
  if (state_.capsuleDetail) return;
  const RootPage next = swipedPage(state_.page, deltaX, locked);
  if (next != state_.page) {
    state_.page = next;
    invalidated_ = true;
  }
}

void Dashboard::swipeVertical(int16_t deltaY, const CapsuleLibrary &library) {
  if (state_.capsuleDetail) {
    if (deltaY < -40) detailLineOffset_ += 8;
    else if (deltaY > 40 && detailLineOffset_ >= 8) detailLineOffset_ -= 8;
  } else if (state_.page == RootPage::capsules) {
    if (deltaY < -40 && listOffset_ + 4 < library.count()) ++listOffset_;
    else if (deltaY > 40 && listOffset_ > 0) --listOffset_;
  }
  invalidated_ = true;
}

bool Dashboard::openCapsuleAt(int16_t y, const CapsuleLibrary &library) {
  if (y < 72 || y >= 412) return false;
  const int index = listOffset_ + (y - 78) / 82;
  if (index < 0 || static_cast<size_t>(index) >= library.count()) return false;
  state_.capsuleSelection = index;
  state_.capsuleDetail = true;
  detailLineOffset_ = 0;
  invalidated_ = true;
  return true;
}

void Dashboard::back() {
  state_.capsuleDetail = false;
  state_.capsuleSelection = -1;
  detailLineOffset_ = 0;
  invalidated_ = true;
}

const CapsuleSummary *Dashboard::selected(const CapsuleLibrary &library) const {
  return state_.capsuleSelection < 0 ? nullptr :
      library.at(static_cast<size_t>(state_.capsuleSelection));
}

}  // namespace pokepod
