#pragma once

#include <Arduino.h>
#include <string.h>

#include "UiSignatureCore.h"

namespace pokepod {

// Allocation-free typed FNV-1a signature for deciding whether a frame changed.
// Length and type-width markers prevent ambiguous concatenations.
class UiSignatureBuilder : public UiSignatureCore {
 public:
  using UiSignatureCore::add;

  void add(const String &value) { addText(value.c_str(), value.length()); }

  void add(const char *value) {
    if (value == nullptr) {
      UiSignatureCore::add(static_cast<uint32_t>(0xffffffffU));
      return;
    }
    addText(value, strlen(value));
  }

};

}  // namespace pokepod
