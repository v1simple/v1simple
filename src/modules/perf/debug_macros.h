#pragma once

/**
 * Debug Macros - Serial alias and compile-time debug switches.
 */

#include <Arduino.h>

// Serial alias for consistent logging
#define SerialLog Serial

// ============================================================================
// Serial Debug Gate (DEBUG_SERIAL)
// Compile-time macro to gate ungated Serial.print/println/printf calls
// Default: disabled (OFF) for production builds
// To enable: add -D DEBUG_SERIAL=1 to platformio.ini build_flags
// ============================================================================
#ifndef DEBUG_SERIAL
#define DEBUG_SERIAL 0  // Disabled by default in production builds
#endif

#if DEBUG_SERIAL
  #define DBG_PRINT(...)   Serial.print(__VA_ARGS__)
  #define DBG_PRINTLN(...) Serial.println(__VA_ARGS__)
  #define DBG_PRINTF(...)  Serial.printf(__VA_ARGS__)
#else
  #define DBG_PRINT(...)   ((void)0)
  #define DBG_PRINTLN(...) ((void)0)
  #define DBG_PRINTF(...)  ((void)0)
#endif
