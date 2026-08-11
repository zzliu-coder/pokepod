#pragma once

namespace pokepod {

// This literal is the single firmware-version source for runtime identity and
// release artifact metadata. Keep the declaration shape stable: the portable
// manifest writer parses it directly instead of maintaining a second value.
constexpr const char kFirmwareVersion[] = "2.0.0";

}  // namespace pokepod
