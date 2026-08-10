#include <cassert>
#include <cstring>

#include "../PokePodAmoled/WifiUiPolicy.h"

int main() {
  using namespace pokepod;

  assert(std::strcmp(wifiUiDetail(WifiPhase::online, true), "已连接") == 0);
  assert(std::strcmp(wifiUiDetail(WifiPhase::connecting, true),
                     "正在连接") == 0);
  assert(std::strcmp(wifiUiDetail(WifiPhase::grace, true),
                     "已连接 · 待机") == 0);
  assert(std::strcmp(wifiUiDetail(WifiPhase::off, true),
                     "省电休眠") == 0);
  assert(std::strcmp(wifiUiDetail(WifiPhase::off, false),
                     "已关闭") == 0);
  assert(std::strcmp(wifiUiDetail(WifiPhase::error, true),
                     "连接失败 · 点按重试") == 0);

  assert(wifiUiSwitchOn(WifiPhase::connecting));
  assert(wifiUiSwitchOn(WifiPhase::online));
  assert(wifiUiSwitchOn(WifiPhase::grace));
  assert(wifiUiSwitchOn(WifiPhase::provisioning));
  assert(!wifiUiSwitchOn(WifiPhase::off));
  assert(!wifiUiSwitchOn(WifiPhase::disabled));
  assert(!wifiUiSwitchOn(WifiPhase::error));

  assert(wifiUiShowsDisconnectedSlash(WifiPhase::off));
  assert(wifiUiShowsDisconnectedSlash(WifiPhase::disabled));
  assert(wifiUiShowsDisconnectedSlash(WifiPhase::error));
  assert(!wifiUiShowsDisconnectedSlash(WifiPhase::connecting));
  assert(!wifiUiShowsDisconnectedSlash(WifiPhase::online));
  assert(!wifiUiShowsDisconnectedSlash(WifiPhase::grace));
  return 0;
}
