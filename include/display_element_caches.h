#pragma once

// ============================================================================
// Display element render caches — unified struct model
//
// Replaces the anonymous file-scoped s_* statics in each display_*.cpp.
// One struct per rendered element; all collected into DisplayElementCaches.
//
// Lifecycle:
//   - Default-initialized to "invalid" (forces first-run full draw)
//   - DisplayElementCaches::invalidateAll() is called from
//     prepareFullRedrawNoClear() after every screen clear
//   - Each render function sets valid = true after a successful draw
//   - Blink timers, font measurement statics, and active slot state
//     are NOT part of this system — they remain file-scoped statics
// ============================================================================

#include <cstdint>
#include "packet_parser.h"   // Band, AlertData

// --- Arrow render cache ---------------------------------------------------
struct ArrowRenderCache {
    bool showFront    = false;
    bool showSide     = false;
    bool showRear     = false;
    // Per-arrow blink-off-phase: true when V1 reports this arrow as flashing
    // (image1 != image2 → flashBits bit set) AND the shared blink phase is in
    // its OFF half.  This is distinct from a non-active "resting" arrow that
    // stays dimly visible as UI chrome. Including these in the cache key forces
    // a repaint at every visible blink transition even though showXxx stays
    // true for the duration of the alert.
    bool blinkOffFront = false;
    bool blinkOffSide  = false;
    bool blinkOffRear  = false;
    bool muted        = false;
    uint16_t frontCol = 0;
    uint16_t sideCol  = 0;
    uint16_t rearCol  = 0;
    bool raisedLayout = true;
    bool valid        = false;

    void invalidate() { valid = false; }
};

// --- Band indicator render cache ------------------------------------------
struct BandRenderCache {
    uint8_t lastMask  = 0xFF;  // 0xFF = undrawn sentinel
    bool lastMuted    = false;
    bool valid        = false;

    void invalidate() { valid = false; }
};

// --- Signal bars render cache ---------------------------------------------
struct BarsRenderCache {
    uint8_t lastStrength = 0xFF;  // 0xFF = undrawn sentinel
    bool lastMuted       = false;
    bool valid           = false;

    void invalidate() { valid = false; }
};

// --- Segment7 frequency render cache --------------------------------------
// NOTE: s_frequencyWidthCache[32] (LRU font measurement) and
//       s_frequencyWidthCacheNextSlot stay as file-scoped statics — they
//       are TextWidthCacheEntry arrays, not render state.
// NOTE: s_frequencyCachedNumericWidth / DashWidth / LaserWidth stay as
//       file-scoped statics — they are font metrics, not render state.
struct FrequencyRenderCache {
    char     lastText[16] = "";
    uint16_t lastColor    = 0;
    bool     lastUsedOfr  = false;
    int      lastDrawX    = 0;
    int      lastDrawWidth = 0;
    bool     valid        = false;

    void invalidate() { valid = false; }
};

// --- Battery render cache -------------------------------------------------
// NOTE: invalidate() resets to sentinel values matching the original
//       dirty.battery behavior: s_batteryLastPctDrawn = -1 and
//       s_batteryLastPctVisible = false forced full redraw. iconValid is
//       also reset so the next icon-mode call repaints the body/cap/cells.
//
// iconValid is exposed for external invalidation (drawProfileIndicator
// clips the bottom-left 11x12 px of the battery body; it flips iconValid
// to false before calling drawBatteryIndicator() so the clipped corner
// gets restored — but only when profile actually redrew).
struct BatteryRenderCache {
    // Percent-mode sub-cache
    int          lastPctDrawn    = -1;     // -1 = undrawn sentinel
    bool         lastPctVisible  = false;
    uint16_t     lastPctColor    = 0;
    unsigned long lastPctDrawMs  = 0;

    // Icon-mode sub-cache
    int          lastFilledSections = -1;  // -1 = undrawn sentinel
    uint16_t     lastFillColor    = 0;
    uint16_t     lastOutlineColor = 0;
    bool         lastIconShown    = false; // true if the icon was last rendered (not just cleared)
    bool         lastOnUSB        = false;
    bool         lastHasBattery   = false;
    bool         lastHideIcon     = false;
    bool         lastShowPercent  = false;
    bool         iconValid        = false; // false forces full icon redraw

    void invalidate() {
        lastPctDrawn = -1;
        lastPctVisible = false;
        lastFilledSections = -1;
        iconValid = false;
    }
};

// --- Top counter render cache (bogey counter + mute icon) -----------------
// NOTE: Both drawTopCounterSegment7 and drawMuteIcon are in display_top_counter.cpp
//       and share this struct. Each function has its own valid flag.
struct TopCounterRenderCache {
    // Bogey counter sub-cache
    char     lastText[8]      = "";
    bool     lastMuted        = false;
    uint16_t lastBogeyColor   = 0;
    bool     counterValid     = false;

    // Mute icon sub-cache
    bool     lastMutedState   = false;
    bool     muteIconValid    = false;

    void invalidate() {
        counterValid  = false;
        muteIconValid = false;
        lastText[0] = '\0';
    }
};

// --- OBD indicator render cache -------------------------------------------
struct ObdRenderCache {
    bool lastShown     = false;
    bool lastConnected = false;
    bool lastAttention = false;
    bool valid         = false;

    void invalidate() { valid = false; }
};

// --- GPS indicator render cache ------------------------------------------
struct GpsRenderCache {
    bool    lastShown = false;
    uint8_t lastSats  = 0xFF;  // 0xFF = undrawn sentinel
    bool    valid     = false;

    void invalidate() { valid = false; }
};

// --- ALP indicator render cache -------------------------------------------
struct AlpRenderCache {
    bool lastShown         = false;
    uint8_t lastState      = 0;   // AlpState as uint8_t for change detection
    uint8_t lastHbByte1    = 0;   // B0 heartbeat byte1 for LISTENING sub-state color
    bool lastAlpEventActive = false; // live ALP event active — drives solid red alert color
    bool valid             = false;

    void invalidate() { valid = false; }
};

// --- Volume indicator render cache ----------------------------------------
// Tracks last-drawn mainVolume/muteVolume + their text colors. The cache is
// keyed off the values that actually influence pixels (the two volume
// numbers and the two user-configurable colors); freshness of BLE context
// is NOT part of the key because drawVolumeIndicator is only called from
// drawStatusStrip when state.supportsVolume() is already true.
struct VolumeRenderCache {
    uint8_t  lastMainVol  = 0xFF;   // 0xFF = undrawn sentinel
    uint8_t  lastMuteVol  = 0xFF;
    uint16_t lastMainColor = 0;
    uint16_t lastMuteColor = 0;
    bool     valid         = false;

    void invalidate() { valid = false; }
};

// --- RSSI indicator render cache ------------------------------------------
// Tracks last-drawn V1 RSSI and app RSSI + the derived color-code bucket
// for each. The hidden/cleared state is tracked separately so toggling
// hideRssiIndicator/hideVolumeIndicator still forces one repaint to clear.
struct RssiRenderCache {
    int      lastV1Rssi     = 1;        // 1 = undrawn sentinel (0 is a valid "not connected" value)
    int      lastAppRssi    = 1;        // 1 = undrawn sentinel
    uint16_t lastV1Color    = 0;
    uint16_t lastAppColor   = 0;
    bool     lastHidden     = false;    // last call rendered the empty/cleared state
    bool     v1LineValid    = false;
    bool     appLineValid   = false;
    bool     valid          = false;

    void invalidate() {
        valid = false;
        v1LineValid = false;
        appLineValid = false;
    }
};

// --- Profile indicator render cache ---------------------------------------
// Tracks last-drawn slot/color/name so the FILL_RECT+text render can be
// skipped when nothing changed. hiddenRender tracks whether we rendered
// the hidden-state (empty rect) vs the shown-state (text) last time,
// because those paths write different pixels.
struct ProfileRenderCache {
    int      lastSlot       = -1;
    uint16_t lastColor      = 0;
    char     lastName[24]   = "";
    bool     hiddenRender   = false;    // last call rendered the empty/cleared state
    bool     valid          = false;

    void invalidate() { valid = false; lastName[0] = '\0'; }
};

// --- WiFi indicator render cache ------------------------------------------
// Tracks last-drawn visibility and color. Arc geometry is a pure function
// of iconSize/wifiX/wifiY (all constants), so color + visibility is a
// complete cache key.
struct WifiRenderCache {
    bool     lastVisible    = false;
    uint16_t lastColor      = 0;
    bool     valid          = false;

    void invalidate() { valid = false; }
};

// --- BLE proxy indicator render cache -------------------------------------
// Tracks last-drawn visibility and color. As with WiFi, rune geometry is
// static so color + drawn-or-hidden is a complete key.
struct BleProxyRenderCache {
    bool     lastDrawn      = false;    // true if icon was last rendered (not cleared)
    uint16_t lastColor      = 0;
    bool     valid          = false;

    void invalidate() { valid = false; }
};

// --- Aggregate -----------------------------------------------------------
struct DisplayElementCaches {
    ArrowRenderCache       arrow;
    BandRenderCache        bands;
    BarsRenderCache        bars;
    FrequencyRenderCache  frequency;
    BatteryRenderCache     battery;
    TopCounterRenderCache  topCounter;
    ObdRenderCache         obd;
    GpsRenderCache         gps;
    AlpRenderCache         alp;
    VolumeRenderCache      volume;
    RssiRenderCache        rssi;
    ProfileRenderCache     profile;
    WifiRenderCache        wifi;
    BleProxyRenderCache    bleProxy;

    /// Call from prepareFullRedrawNoClear() after every screen clear.
    void invalidateAll() {
        arrow.invalidate();
        bands.invalidate();
        bars.invalidate();
        frequency.invalidate();
        battery.invalidate();
        topCounter.invalidate();
        obd.invalidate();
        gps.invalidate();
        alp.invalidate();
        volume.invalidate();
        rssi.invalidate();
        profile.invalidate();
        wifi.invalidate();
        bleProxy.invalidate();
    }
};
