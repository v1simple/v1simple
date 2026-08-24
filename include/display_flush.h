// display_flush.h — Shared DISPLAY_FLUSH macro for display_*.cpp files
// Flushes the Arduino_GFX framebuffer when a display is attached.
#pragma once


#define DISPLAY_FLUSH()                                                                                                \
    do {                                                                                                               \
        if (tft_) {                                                                                                    \
            tft_->flush();                                                                                             \
        }                                                                                                              \
    } while (0)
