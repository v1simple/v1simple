/**
 * Indicator badges & frame — extracted from display.cpp (Phase 2P)
 *
 * Contains drawBaseFrame, drawStatusText, and associated setters.
 */

#include "display.h"
#include <cstring>
#include "display_draw.h"
#include "display_dirty_flags.h"
#include "display_element_caches.h"
#include "display_palette.h"
#include "display_text.h"
#include "settings.h"
#include "perf_metrics.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/alp/alp_runtime_module.h"
#include "modules/alp/alp_laser_event.h"
#include "modules/display/display_edge_log.h"
#include "main_globals.h"
#include "modules/gps/gps_runtime_module.h"

// ============================================================================
// Base frame
// ============================================================================

void V1Display::drawBaseFrame() {
    // Clean black background (t4s3-style)
    TFT_CALL(fillScreen)(PALETTE_BG);
    prepareFullRedrawNoClear();
}

void V1Display::prepareFullRedrawNoClear() {
    bleProxyDrawn_ = false;  // Force indicator redraw after full clears
    dirty_.setIndicatorFlags();  // Sets obdIndicator + alpIndicator only; other fields untouched
    elementCaches_.invalidateAll();     // Directly zeros all per-element render caches
    drawBLEProxyIndicator();  // Redraw BLE icon after screen clear
}

void V1Display::setSpeedVolZeroActive(bool active) {
    speedVolZeroActive_ = active;
}

// OBD indicator render cache is in elementCaches_.obd

// ============================================================================
// Status indicators
// ============================================================================

void V1Display::setObdStatus(bool enabled, bool connected, bool scanAttention) {
    obdEnabled_ = enabled;
    obdConnected_ = connected;
    obdScanAttention_ = scanAttention;
}

void V1Display::setObdAttention(bool attention) {
    if (obdAttention_ == attention) {
        return;
    }
    obdAttention_ = attention;
    dirty_.obdIndicator = true;
    elementCaches_.obd.invalidate();   // Direct cache invalidation at the source
}

void V1Display::setObdRuntimeModule(ObdRuntimeModule* m) {
    obdRtMod_ = m;
}

void V1Display::refreshObdIndicator(uint32_t nowMs) {
    syncTopIndicators(nowMs);
    drawObdIndicator();
}

void V1Display::syncTopIndicators(uint32_t nowMs) {
    if (previewIndicatorOverridesActive_) {
        return;
    }
    if (obdRtMod_) {
        const ObdRuntimeStatus obdStatus = obdRtMod_->snapshot(nowMs);
        setObdStatus(obdStatus.enabled,
                     obdStatus.connected,
                     obdStatus.scanInProgress || obdStatus.manualScanPending);
    }
    if (alpRtMod_) {
        const AlpStatus alpStatus = alpRtMod_->snapshot();
        // Badge context only — event-owned fields are updated via setAlpLaserEvent() (Phase 2)
        alpEnabled_ = (alpStatus.state != AlpState::OFF);
        alpStateRaw_ = static_cast<uint8_t>(alpStatus.state);
        alpHbByte1_ = alpStatus.lastHbByte1;
        // Note: alpHasLaserEvent_, alpLaserEvent_, alpFreqOverride_, alpFreqText_,
        // alpLidActive_ are no longer written here. They are written atomically by
        // setAlpLaserEvent() from the display pipeline.
    }
}

// ============================================================================
// OBD indicator ("OBD" text badge)
// ============================================================================

void V1Display::drawObdIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    const bool wantShow = obdEnabled_;
    const bool curConnected = wantShow && obdConnected_;
    const bool curAttention = wantShow && !curConnected && (obdScanAttention_ || obdAttention_);

    if (!dirty_.obdIndicator &&
        elementCaches_.obd.valid &&
        wantShow == elementCaches_.obd.lastShown &&
        curConnected == elementCaches_.obd.lastConnected &&
        curAttention == elementCaches_.obd.lastAttention) {
        return;
    }
    dirty_.obdIndicator = false;     // Still cleared here — it's also read externally for flush
    elementCaches_.obd.valid = true;
    elementCaches_.obd.lastShown = wantShow;
    elementCaches_.obd.lastConnected = curConnected;
    elementCaches_.obd.lastAttention = curAttention;

    const DisplayLayout::DisplayRect badgeRect = DisplayLayout::obdBadgeRect();

    perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Obd);
    drawnRegion_.add(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h,
                         DisplayDirtyRegionSource::Indicators);
    FILL_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, PALETTE_BG);
    if (!wantShow) {
        return;
    }

    const V1Settings& s = settingsManager.get();
    const uint16_t textColor = curConnected ? s.colorObd : (curAttention ? 0xF800 : s.colorMuted);

    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(textColor, PALETTE_BG);
    GFX_drawString(tft_, "OBD",
                   badgeRect.x + badgeRect.w / 2,
                   badgeRect.y + badgeRect.h / 2);
#endif
}

// ============================================================================
// GPS indicator ("G7" satellite count badge — left of ALP badge)
// ============================================================================

void V1Display::drawGpsIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    const GpsRuntimeStatus gs = gpsRuntimeModule.snapshot(millis());
    const bool wantShow = gs.enabled && gs.stableHasFix;
    const uint8_t sats = wantShow ? gs.stableSatellites : 0;

    if (!dirty_.gpsIndicator &&
        elementCaches_.gps.valid &&
        wantShow == elementCaches_.gps.lastShown &&
        sats == elementCaches_.gps.lastSats) {
        return;
    }
    dirty_.gpsIndicator = false;
    elementCaches_.gps.valid    = true;
    elementCaches_.gps.lastShown = wantShow;
    elementCaches_.gps.lastSats  = sats;

    const DisplayLayout::DisplayRect badgeRect = DisplayLayout::gpsBadgeRect();

    perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Gps);
    drawnRegion_.add(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h,
                         DisplayDirtyRegionSource::Indicators);
    FILL_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, PALETTE_BG);
    if (!wantShow) {
        return;
    }

    char buf[6];
    snprintf(buf, sizeof(buf), "G%u", sats);
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(0x07E0, PALETTE_BG);  // Green — stable GPS fix
    GFX_drawString(tft_, buf,
                   badgeRect.x + badgeRect.w / 2,
                   badgeRect.y + badgeRect.h / 2);
#endif
}

// ============================================================================
// ALP indicator ("ALP" text badge — left of MUTED badge)
// ============================================================================

void V1Display::setAlpRuntimeModule(AlpRuntimeModule* m) {
    alpRtMod_ = m;
}

void V1Display::refreshAlpIndicator(uint32_t nowMs) {
    if (previewIndicatorOverridesActive_) {
        return;
    }
    if (!alpRtMod_) return;
    const AlpStatus status = alpRtMod_->snapshot();
    // Badge context only — event-owned fields are updated via setAlpLaserEvent() (Phase 2)
    alpEnabled_ = (status.state != AlpState::OFF);
    alpStateRaw_ = static_cast<uint8_t>(status.state);
    alpHbByte1_ = status.lastHbByte1;
    // Note: alpHasLaserEvent_ is no longer written here. It is written atomically by
    // setAlpLaserEvent() from the display pipeline.
    drawAlpIndicator();
}

void V1Display::drawAlpIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    const bool wantShow = alpEnabled_;

    if (!dirty_.alpIndicator &&
        elementCaches_.alp.valid &&
        wantShow == elementCaches_.alp.lastShown &&
        alpStateRaw_ == elementCaches_.alp.lastState &&
        alpHbByte1_ == elementCaches_.alp.lastHbByte1 &&
        alpHasLaserEvent_ == elementCaches_.alp.lastAlpEventActive) {
        return;
    }
    dirty_.alpIndicator = false;
    elementCaches_.alp.valid = true;
    elementCaches_.alp.lastShown = wantShow;
    elementCaches_.alp.lastState = alpStateRaw_;
    elementCaches_.alp.lastHbByte1 = alpHbByte1_;
    elementCaches_.alp.lastAlpEventActive = alpHasLaserEvent_;

    const DisplayLayout::DisplayRect badgeRect = DisplayLayout::alpBadgeRect();

    perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Alp);
    drawnRegion_.add(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h,
                         DisplayDirtyRegionSource::Indicators);
    FILL_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, PALETTE_BG);
    if (!wantShow) {
        return;
    }

    // Badge colors match ALP control pad LED — including LISTENING sub-states
    // driven by B0 heartbeat byte1 (speed-gated by ALP's internal GPS):
    // Color rule — session-first, then mode:
    //   Grey  — OFF / IDLE: not enabled, or UART timed out
    //   Red   — ALP is actively alerting right now (live laser event,
    //           not Warm-Up). When the live alert stops, the badge drops
    //           back to the mode color instead of hanging onto stale gun /
    //           laser state through TEARDOWN.
    //   Blue  — LISTENING byte1=04: LID active (above LID speed limit)
    //   Orange— LISTENING byte1=03: DLI active (below LID speed limit)
    //   Green — LISTENING byte1=02 / 00: Warm-Up / pre-Warm-Up
    //
    // Rationale: the display follows live alert ownership only. byte1 drives
    // the mode colors whenever no live laser event is active.
    const V1Settings& s = settingsManager.get();
    const AlpState alpState = static_cast<AlpState>(alpStateRaw_);
    uint16_t textColor;
    if (alpState == AlpState::OFF || alpState == AlpState::IDLE) {
        textColor = s.colorMuted;              // Grey — not connected
    } else if (alpHasLaserEvent_) {
        textColor = s.colorAlpAlert;           // Red — active laser alert
    } else {
        switch (alpHbByte1_) {
            case 0x04:
                textColor = s.colorAlpLidActive; // Blue — LID active (above LID speed limit)
                break;
            case 0x03:
                textColor = s.colorAlpDli;       // Orange — DLI active (below LID speed limit)
                break;
            case 0x02:
            case 0x00:
                textColor = s.colorAlpConnected; // Green — warm-up / pre-warm-up
                break;
            default:
                textColor = s.colorAlpConnected; // Green — unknown byte1, treat as idle
                break;
        }
    }

    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(textColor, PALETTE_BG);
    GFX_drawString(tft_, "ALP",
                   badgeRect.x + badgeRect.w / 2,
                   badgeRect.y + badgeRect.h / 2);
#endif
}

// ============================================================================
// Preview-mode direct setters (bypass runtime modules for display test)
// ============================================================================

void V1Display::setPreviewIndicatorOverridesActive(bool active) {
    if (previewIndicatorOverridesActive_ == active) {
        return;
    }

    previewIndicatorOverridesActive_ = active;
    // Force the first preview-owned badge draw and the first post-preview
    // runtime-owned badge draw to repaint instead of cache-hitting against
    // whichever owner rendered the previous frame.
    dirty_.obdIndicator = true;
    dirty_.alpIndicator = true;
    elementCaches_.obd.invalidate();
    elementCaches_.alp.invalidate();
}

void V1Display::setAlpPreviewState(bool enabled, uint8_t state, uint8_t hbByte1) {
    const AlpState previewState = static_cast<AlpState>(state);
    const bool nextHasLaserEvent = (previewState == AlpState::ALERT_ACTIVE ||
                                   previewState == AlpState::NOISE_WINDOW ||
                                   previewState == AlpState::TEARDOWN);

    if (alpEnabled_ == enabled &&
        alpStateRaw_ == state &&
        alpHbByte1_ == hbByte1 &&
        alpHasLaserEvent_ == nextHasLaserEvent &&
        alpLaserEvent_.active == nextHasLaserEvent &&
        alpLaserEvent_.gun == AlpGunType::UNKNOWN &&
        alpLaserEvent_.direction == AlpLaserDirection::UNKNOWN &&
        !alpLaserEvent_.lidActive) {
        return;
    }

    alpEnabled_ = enabled;
    alpStateRaw_ = state;
    alpHbByte1_ = hbByte1;
    // Preview mode: derive hasLaserEvent from the previewed state, so the
    // red-alert color renders correctly during display tests. Alert states
    // (ALERT_ACTIVE, NOISE_WINDOW, TEARDOWN) count as "has laser event";
    // everything else is mode-colored by byte1.
    alpHasLaserEvent_ = nextHasLaserEvent;
    alpLaserEvent_.active = alpHasLaserEvent_;
    alpLaserEvent_.gun = AlpGunType::UNKNOWN;
    alpLaserEvent_.direction = AlpLaserDirection::UNKNOWN;
    alpLaserEvent_.lidActive = false;
    dirty_.alpIndicator = true;
    elementCaches_.alp.invalidate();
}

void V1Display::setObdPreviewState(bool enabled, bool connected, bool scanAttention) {
    if (obdEnabled_ == enabled &&
        obdConnected_ == connected &&
        obdScanAttention_ == scanAttention) {
        return;
    }

    setObdStatus(enabled, connected, scanAttention);
    dirty_.obdIndicator = true;
    elementCaches_.obd.invalidate();
}

// ============================================================================
// ALP frequency-area override
// ============================================================================

// ============================================================================
// ALP laser event — atomic writer (Phase 2)
// ============================================================================

void V1Display::setAlpLaserEvent(const AlpLaserEvent& ev) {
    const bool prevActive = alpHasLaserEvent_;
    const bool prevLidActive = alpLidActive_;
    const uint8_t prevDirection = static_cast<uint8_t>(alpLaserEvent_.direction);
    const bool prevFreqOverride = alpFreqOverride_;
    const AlpGunType prevGun = alpLaserEvent_.gun;
    const bool holdLiveGun = prevActive && ev.active &&
                             ev.gun == AlpGunType::UNKNOWN &&
                             prevGun != AlpGunType::UNKNOWN;
    const AlpGunType effectiveGun = holdLiveGun ? prevGun : ev.gun;
    // A persisted ALP tail is allowed to keep gun text/direction while no
    // longer counting as a live ALP event. active gates live-only visuals;
    // gun presence gates the frequency-area override.
    const bool nextFreqOverride = (effectiveGun != AlpGunType::UNKNOWN);
    const char* nextGunAbbr = nextFreqOverride ? alpGunAbbrev(effectiveGun) : "";
    const bool nextLidActive = nextFreqOverride ? ev.lidActive : false;
    const bool gunChanged = (prevFreqOverride != nextFreqOverride) ||
                            (nextFreqOverride &&
                             strncmp(alpFreqText_, nextGunAbbr, sizeof(alpFreqText_)) != 0);
    const bool visualChanged = (ev.active != prevActive) ||
                               (static_cast<uint8_t>(ev.direction) != prevDirection) ||
                               gunChanged ||
                               (nextLidActive != prevLidActive);

    // Atomic write of all ALP event-owned fields
    alpLaserEvent_ = ev;
    alpLaserEvent_.gun = effectiveGun;
    alpHasLaserEvent_ = ev.active;

    if (nextFreqOverride) {
        alpFreqOverride_ = true;
        strncpy(alpFreqText_, nextGunAbbr, sizeof(alpFreqText_));
        alpFreqText_[sizeof(alpFreqText_) - 1] = '\0';
        alpLidActive_ = nextLidActive;
    } else {
        alpFreqOverride_ = false;
        alpFreqText_[0] = '\0';
        alpLidActive_ = false;
    }

    // Invalidate caches on change
    if (visualChanged) {
        elementCaches_.alp.invalidate();
        elementCaches_.arrow.invalidate();
        elementCaches_.frequency.invalidate();
    }

    // Edge log on change (matches prior edge-log behavior from pipeline)
    if (visualChanged) {
        char detail[48];
        const char* gunStr = nextFreqOverride ? nextGunAbbr : "none";
        const char* dirStr = "UNKNOWN";
        if (ev.direction == AlpLaserDirection::FRONT) dirStr = "FRONT";
        else if (ev.direction == AlpLaserDirection::REAR) dirStr = "REAR";
        snprintf(detail, sizeof(detail), "active=%d gun=%s dir=%s lid=%d",
                 ev.active ? 1 : 0, gunStr, dirStr, nextLidActive ? 1 : 0);
        if (alpRtMod_) {
            alpRtMod_->logDisplayDecision(millis(), "DISP_V1_EVENT", detail);
        }
    }
}

void V1Display::setAlpFrequencyOverride(const char* gunAbbrev, bool lidActive) {
    if (!gunAbbrev) {
        clearAlpFrequencyOverride();
        return;
    }
    const bool detailChanged = !alpFreqOverride_ ||
                               alpLidActive_ != lidActive ||
                               strncmp(alpFreqText_, gunAbbrev, sizeof(alpFreqText_)) != 0;
    alpFreqOverride_ = true;
    alpLidActive_ = lidActive;
    strncpy(alpFreqText_, gunAbbrev, sizeof(alpFreqText_));
    alpFreqText_[sizeof(alpFreqText_) - 1] = '\0';

    if (detailChanged) {
        char detail[32];
        snprintf(detail, sizeof(detail), "%s lid=%d", gunAbbrev, lidActive ? 1 : 0);
        logV1DisplaySetterEdge(millis(), "setAlpFrequencyOverride", detail);
    }
}

void V1Display::clearAlpFrequencyOverride() {
    const bool wasOverride = alpFreqOverride_;
    alpFreqOverride_ = false;
    alpLidActive_ = false;
    alpFreqText_[0] = '\0';

    // Edge log if this is a transition from override to no override
    if (wasOverride) {
        logV1DisplaySetterEdge(millis(), "clearAlpFrequencyOverride", "");
    }
}

// ============================================================================
// Status text (centered message)
// ============================================================================

void V1Display::drawStatusText(const char* text, uint16_t color) {
    TFT_CALL(setTextColor)(color, PALETTE_BG);
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    GFX_drawString(tft_, text, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
}
