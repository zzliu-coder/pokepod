#include "FirmwareImageIdentity.h"

namespace pokepod {

#if defined(__APPLE__)
#define POKEPOD_IMAGE_IDENTITY_ATTRIBUTE __attribute__((used, aligned(4)))
#else
#define POKEPOD_IMAGE_IDENTITY_ATTRIBUTE \
  __attribute__((used, section(".rodata.pokepod_identity"), aligned(4)))
#endif

const FirmwareImageIdentity kFirmwareImageIdentity
    POKEPOD_IMAGE_IDENTITY_ATTRIBUTE =
        makeFirmwareImageIdentity();

#undef POKEPOD_IMAGE_IDENTITY_ATTRIBUTE

}  // namespace pokepod
