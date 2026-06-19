/**
 * Display Driver Abstraction Layer
 * Waveshare ESP32-S3-Touch-LCD-3.49 (AXS15231B, Arduino_GFX)
 */

#pragma once

// Waveshare-only build
#define DISPLAY_USE_ARDUINO_GFX 1
#define DISPLAY_NAME "Waveshare 3.49\""

// Screen dimensions - can be overridden by build flags
// These are VISUAL dimensions (what we draw to)
#ifndef SCREEN_WIDTH
    // Waveshare 3.49" in landscape mode (visual)
    #define SCREEN_WIDTH 640
    #define SCREEN_HEIGHT 172
#endif

#ifndef SCREEN_HEIGHT
    #define SCREEN_HEIGHT 172
#endif

// Canvas physical dimensions (before rotation)
#define CANVAS_WIDTH 172
#define CANVAS_HEIGHT 640

// Include appropriate graphics library
    #include <Arduino_GFX_Library.h>

    // Pin definitions with defaults for Waveshare 3.49"
    #ifndef LCD_CS
        #define LCD_CS 9
    #endif
    #ifndef LCD_SCLK
        #define LCD_SCLK 10
    #endif
    #ifndef LCD_DATA0
        #define LCD_DATA0 11
    #endif
    #ifndef LCD_DATA1
        #define LCD_DATA1 12
    #endif
    #ifndef LCD_DATA2
        #define LCD_DATA2 13
    #endif
    #ifndef LCD_DATA3
        #define LCD_DATA3 14
    #endif
    #ifndef LCD_RST
        #define LCD_RST 21
    #endif
    #ifndef LCD_BL
        #define LCD_BL 8
    #endif

    // TFT_eSPI compatibility defines for Arduino_GFX
    // Text datum (alignment) constants
    #define TL_DATUM 0  // Top Left
    #define TC_DATUM 1  // Top Center
    #define TR_DATUM 2  // Top Right
    #define ML_DATUM 3  // Middle Left
    #define MC_DATUM 4  // Middle Center
    #define MR_DATUM 5  // Middle Right
    #define BL_DATUM 6  // Bottom Left
    #define BC_DATUM 7  // Bottom Center
    #define BR_DATUM 8  // Bottom Right

    // Color aliases for compatibility
    #define TFT_BLACK     0x0000
    #define TFT_WHITE     0xFFFF
    #define TFT_RED       0xF800
    #define TFT_GREEN     0x07E0
    #define TFT_BLUE      0x001F
    #define TFT_DARKGREY  0x1082  // Very dark grey
    #define TFT_LIGHTGREY 0xC618
    #define TFT_YELLOW    0xFFE0
    #define TFT_ORANGE    0xFD20


// Common color definitions (RGB565)
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_ORANGE  0xFD20
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F

