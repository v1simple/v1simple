#pragma once

// Forced redraw and mode flags shared by display renderers.
struct DisplayDirtyFlags {
    bool multiAlert = false;    // Layout mode flag (not element cache)
    bool cards = false;         // Force-redraw signal set from display_update.cpp
    bool obdIndicator = false;  // Force-redraw signal for OBD indicator flush
    bool gpsIndicator = false;  // Force-redraw signal for GPS indicator flush
    bool alpIndicator = false;  // Force-redraw signal for ALP indicator flush
    bool resetTracking = false; // Signals element cache and tracking state reset

    // Force external indicator flushes without changing pipeline-owned mode flags.
    // prepareFullRedrawNoClear() invalidates element caches separately.
    void setIndicatorFlags() {
        obdIndicator = true;
        gpsIndicator = true;
        alpIndicator = true;
    }
};
