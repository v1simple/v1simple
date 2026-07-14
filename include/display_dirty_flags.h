#pragma once

// ============================================================================
// DisplayDirtyFlags — tracks which display elements need a forced redraw
// after a full screen clear or mode change.
//
// Extracted from display.cpp so that display sub-modules (display_arrow.cpp,
// etc.) can read/write the shared dirty-flag aggregate.
// ============================================================================
struct DisplayDirtyFlags {
    bool multiAlert = false;    // Layout mode flag (not element cache)
    bool cards = false;         // Force-redraw signal set from display_update.cpp
    bool obdIndicator = false;  // Force-redraw signal for OBD indicator flush
    bool gpsIndicator = false;  // Force-redraw signal for GPS indicator flush
    bool alpIndicator = false;  // Force-redraw signal for ALP indicator flush
    bool resetTracking = false; // Signals element cache and tracking state reset

    /// Mark the residual indicator flags (OBD, GPS, ALP) that gate external
    /// flush routing after a full screen clear.  Name intentionally narrower
    /// than the historical setAll(): this method does NOT touch multiAlert,
    /// or resetTracking — those are mode flags owned by the render pipeline,
    /// not indicator-level invalidation signals.  Element-level
    /// invalidation is handled by g_elementCaches.invalidateAll() called
    /// from prepareFullRedrawNoClear() alongside this function.
    void setIndicatorFlags() {
        obdIndicator = true; // Still read externally for flush routing
        gpsIndicator = true;
        alpIndicator = true;
    }
};
