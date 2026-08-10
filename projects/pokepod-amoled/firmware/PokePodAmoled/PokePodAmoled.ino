#include <Arduino.h>

// Arduino CLI regenerates and recompiles the primary .ino translation unit on
// every invocation. Keep it deliberately tiny; setup() and loop() live in
// PokePodApp.cpp so unchanged application code can reuse its object file.
