#pragma once

#include "BleVoiceCallbackMailbox.h"

namespace pokepod {

inline bool bleCallbackAllowedDuringDisable(
    const BleVoiceCallbackEvent &event, bool hasCurrent,
    uint16_t currentConnectionId, uint32_t currentGeneration) {
  if (event.type == BleVoiceCallbackEventType::connect) return true;
  if (!hasCurrent || currentGeneration == 0 ||
      event.connectionId != currentConnectionId ||
      event.connectionGeneration != currentGeneration) {
    return false;
  }
  return event.type == BleVoiceCallbackEventType::disconnect ||
      event.type == BleVoiceCallbackEventType::command ||
      event.type == BleVoiceCallbackEventType::audioNotifyStatus ||
      event.type == BleVoiceCallbackEventType::controlNotifyStatus;
}

}  // namespace pokepod
