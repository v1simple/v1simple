#pragma once

// Renderers mark caches valid after drawing. Screen clears invalidate them via
// prepareFullRedrawNoClear(); font metrics and blink timing survive that reset.

#include <cstdint>
#include "packet_parser.h" // Band, AlertData

struct ArrowRenderCache {
    bool showFront = false;
    bool showSide = false;
    bool showRear = false;
    // Blink-off erases an active arrow; an inactive arrow stays dim. Cache the
    // phase separately because showXxx stays true through the whole alert.
    bool blinkOffFront = false;
    bool blinkOffSide = false;
    bool blinkOffRear = false;
    bool muted = false;
    uint16_t frontCol = 0;
    uint16_t sideCol = 0;
    uint16_t rearCol = 0;
    bool raisedLayout = true;
    bool valid = false;

    void invalidate() { valid = false; }
};

// Palette revision prevents stale colors even when forceNextRedraw() was skipped.
struct BandRenderCache {
    uint8_t lastMask = 0xFF;       // Effective mask; Ku identity remains set while its shared K cell blinks off.
    uint8_t lastFlashBits = 0;     // Needed when a card clear restores the overlapping band label.
    bool lastKuBlinkOff = false;   // Distinguishes active/dim Ku when callers provide Ku without the physical K bit.
    bool lastMuted = false;
    uint32_t lastPaletteRevision = 0;
    bool valid = false;

    void invalidate() { valid = false; }
};

struct BarsRenderCache {
    uint8_t lastStrength = 0xFF; // 0xFF = undrawn sentinel
    bool lastMuted = false;
    uint32_t lastPaletteRevision = 0;
    bool valid = false;

    void invalidate() { valid = false; }
};

struct FrequencyRenderCache {
    char lastText[16] = "";
    uint16_t lastColor = 0;
    bool lastUsedOfr = false;
    int lastDrawX = 0;
    int lastDrawWidth = 0;
    bool valid = false;

    void invalidate() { valid = false; }
};

// Profile drawing clips the battery corner and clears iconValid to restore it.
struct BatteryRenderCache {
    // Percent-mode sub-cache
    int lastPctDrawn = -1; // -1 = undrawn sentinel
    bool lastPctVisible = false;
    uint16_t lastPctColor = 0;
    unsigned long lastPctDrawMs = 0;

    // Icon-mode sub-cache
    int lastFilledSections = -1; // -1 = undrawn sentinel
    uint16_t lastFillColor = 0;
    uint16_t lastOutlineColor = 0;
    bool lastIconShown = false; // true if the icon was last rendered (not just cleared)
    bool lastOnUSB = false;
    bool lastHasBattery = false;
    bool lastHideIcon = false;
    bool lastShowPercent = false;
    bool iconValid = false; // false forces full icon redraw

    void invalidate() {
        lastPctDrawn = -1;
        lastPctVisible = false;
        lastFilledSections = -1;
        iconValid = false;
    }
};

// The counter and mute badge invalidate independently.
struct TopCounterRenderCache {
    // Bogey counter sub-cache
    char lastText[8] = "";
    bool lastMuted = false;
    bool lastFixedPrimaryDot = false;
    uint16_t lastBogeyColor = 0;
    bool counterValid = false;

    // Mute icon sub-cache
    bool lastMutedState = false;
    bool muteIconValid = false;

    void invalidate() {
        counterValid = false;
        muteIconValid = false;
        lastText[0] = '\0';
    }
};

struct ObdRenderCache {
    bool lastShown = false;
    bool lastConnected = false;
    bool lastAttention = false;
    bool valid = false;

    void invalidate() { valid = false; }
};

struct GpsRenderCache {
    bool lastShown = false;
    uint8_t lastSats = 0xFF; // 0xFF = undrawn sentinel
    bool valid = false;

    void invalidate() { valid = false; }
};

struct AlpRenderCache {
    bool lastShown = false;
    uint8_t lastState = 0;           // AlpState as uint8_t for change detection
    uint8_t lastHbByte1 = 0;         // B0 heartbeat byte1 for LISTENING sub-state color
    bool lastAlpEventActive = false; // live ALP event active — drives solid red alert color
    bool valid = false;

    void invalidate() { valid = false; }
};

// Screen clears must also discard slot identities, or grace matching can
// resurrect cards that no longer exist in the framebuffer.
struct CardSlot {
    AlertData alert{};
    unsigned long lastSeen = 0;
};

struct CardDrawnPosition {
    Band band = BAND_NONE;
    uint32_t frequency = 0;
    uint8_t direction = 0;
    uint8_t photoType = 0;
    bool isGraced = false;
    bool wasMuted = false;
    uint8_t bars = 0;
};

struct CardsRenderCache {
    CardSlot slots[2]{};
    CardDrawnPosition lastDrawnPositions[2]{};
    AlertData lastPriority{};
    int lastDrawnCount = 0;
    int lastProfileSlot = -1;
    bool forceRedraw = true;

    void invalidate() {
        for (int i = 0; i < 2; ++i) {
            slots[i].alert = AlertData();
            slots[i].lastSeen = 0;
            lastDrawnPositions[i] = CardDrawnPosition();
        }
        lastPriority = AlertData();
        lastDrawnCount = 0;
        lastProfileSlot = -1;
        forceRedraw = true;
    }
};

// BLE freshness is not a key: drawStatusStrip gates this on supportsVolume().
struct VolumeRenderCache {
    uint8_t lastMainVol = 0xFF; // 0xFF = undrawn sentinel
    uint8_t lastMuteVol = 0xFF;
    uint16_t lastMainColor = 0;
    uint16_t lastMuteColor = 0;
    bool valid = false;

    void invalidate() { valid = false; }
};

// Track hidden state so visibility changes clear the old pixels once.
struct RssiRenderCache {
    int lastV1Rssi = 1;  // 1 = undrawn sentinel (0 is a valid "not connected" value)
    int lastAppRssi = 1; // 1 = undrawn sentinel
    uint16_t lastV1Color = 0;
    uint16_t lastAppColor = 0;
    bool lastHidden = false; // last call rendered the empty/cleared state
    bool v1LineValid = false;
    bool appLineValid = false;
    bool valid = false;

    void invalidate() {
        valid = false;
        v1LineValid = false;
        appLineValid = false;
    }
};

struct ProfileRenderCache {
    int lastSlot = -1;
    uint16_t lastColor = 0;
    char lastName[24] = "";
    bool hiddenRender = false; // last call rendered the empty/cleared state
    bool valid = false;

    void invalidate() {
        valid = false;
        lastName[0] = '\0';
    }
};

// WiFi and BLE icon geometry is fixed; visibility and color are complete keys.
struct WifiRenderCache {
    bool lastVisible = false;
    uint16_t lastColor = 0;
    bool valid = false;

    void invalidate() { valid = false; }
};

struct BleProxyRenderCache {
    bool lastDrawn = false; // true if icon was last rendered (not cleared)
    uint16_t lastColor = 0;
    bool valid = false;

    void invalidate() { valid = false; }
};

struct DisplayElementCaches {
    ArrowRenderCache arrow;
    BandRenderCache bands;
    BarsRenderCache bars;
    FrequencyRenderCache frequency;
    BatteryRenderCache battery;
    TopCounterRenderCache topCounter;
    ObdRenderCache obd;
    GpsRenderCache gps;
    AlpRenderCache alp;
    CardsRenderCache cards;
    VolumeRenderCache volume;
    RssiRenderCache rssi;
    ProfileRenderCache profile;
    WifiRenderCache wifi;
    BleProxyRenderCache bleProxy;

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
        cards.invalidate();
        volume.invalidate();
        rssi.invalidate();
        profile.invalidate();
        wifi.invalidate();
        bleProxy.invalidate();
    }
};
