#pragma once

namespace pokepod {

inline bool usbPhysicalConnected(bool tinyUsbMounted, bool pmuAvailable,
                                 bool vbusPresent) {
  // VBUS is the authoritative cable signal on a healthy AXP2101. Preserve a
  // mounted-only fallback so a PMU probe failure does not hide a usable link.
  return tinyUsbMounted && (!pmuAvailable || vbusPresent);
}

}  // namespace pokepod
