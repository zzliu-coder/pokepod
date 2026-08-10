#pragma once

#include "WifiPolicy.h"

namespace pokepod {

inline const char *wifiUiDetail(WifiPhase phase, bool automaticEnabled) {
  switch (phase) {
    case WifiPhase::online: return "已连接";
    case WifiPhase::connecting: return "正在连接";
    case WifiPhase::grace: return "已连接 · 待机";
    case WifiPhase::provisioning: return "配网中";
    case WifiPhase::error: return "连接失败 · 点按重试";
    case WifiPhase::off:
      return automaticEnabled ? "省电休眠" : "已关闭";
    case WifiPhase::disabled: return "尚未配网";
  }
  return "";
}

inline bool wifiUiSwitchOn(WifiPhase phase) {
  return phase == WifiPhase::connecting || phase == WifiPhase::online ||
      phase == WifiPhase::grace || phase == WifiPhase::provisioning;
}

inline bool wifiUiShowsDisconnectedSlash(WifiPhase phase) {
  return phase == WifiPhase::disabled || phase == WifiPhase::off ||
      phase == WifiPhase::error;
}

}  // namespace pokepod
