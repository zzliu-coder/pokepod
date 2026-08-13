#pragma once

#include "SecureWipe.h"

namespace pokepod {

template <typename Settings>
inline void secureWipeSecrets(Settings &settings) {
  secureWipe(settings.wifiPassword);
  secureWipe(settings.secretId);
  secureWipe(settings.secretKey);
}

}  // namespace pokepod
