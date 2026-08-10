#pragma once

// PokePod only uses one QSPI bus, two compatible AMOLED controllers and the
// indexed canvas. Including the upstream umbrella header makes Arduino CLI
// discover and compile more than 200 unrelated display drivers.
#include <Arduino_DataBus.h>
#include <Arduino_GFX.h>
#include <Arduino_TFT.h>
#include <Arduino_OLED.h>
#include <canvas/Arduino_Canvas_Indexed.h>
#include <databus/Arduino_ESP32QSPI.h>
#include <display/Arduino_CO5300.h>
#include <display/Arduino_SH8601.h>
