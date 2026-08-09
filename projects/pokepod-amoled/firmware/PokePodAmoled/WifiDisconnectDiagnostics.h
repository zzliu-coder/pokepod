#pragma once

#include <cstdint>

namespace pokepod {

void beginWifiDisconnectDiagnostics();
void clearWifiDisconnectReason();
uint16_t lastWifiDisconnectReason();

}  // namespace pokepod
