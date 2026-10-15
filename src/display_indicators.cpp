#include "display.h"
#include <cstring>
#include "display_draw.h"
#include "display_dirty_flags.h"
#include "display_element_caches.h"
#include "display_palette.h"
#include "display_text.h"
#include "settings.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/alp/alp_runtime_module.h"
#include "modules/alp/alp_laser_event.h"
#include "modules/gps/gps_runtime_module.h"

void V1Display::drawBaseFrame() {
    TFT_CALL(fillScreen)(PALETTE_BG);
    prepareFullRedrawNoClear();
}

void V1Display::prepareFullRedrawNoClear() {
    bleProxyDrawn_ = false;         // Force indicator redraw after full clears
    dirty_.setIndicatorFlags();     // Sets obdIndicator + alpIndicator only; other fields untouched
    elementCaches_.invalidateAll(); // Directly zeros all per-element render caches
    drawBLEProxyIndicator();        // Redraw BLE icon after screen clear
}

void V1Display::setSpeedVolZeroActive(bool active) {
    speedVolZeroActive_ = active;
}

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
    elementCaches_.obd.invalidate(); // Direct cache invalidation at the source
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
        setObdStatus(obdStatus.enabled, obdStatus.connected, obdStatus.scanInProgress || obdStatus.manualScanPending);
    }
    if (alpRtMod_) {
        const AlpStatus alpStatus = alpRtMod_->snapshot();
        // The runtime snapshot owns badge context; the display pipeline owns
        // the cohesive laser-event fields set by setAlpLaserEvent().
        alpEnabled_ = (alpStatus.state != AlpState::OFF);
        alpStateRaw_ = static_cast<uint8_t>(alpStatus.state);
        alpHbByte1_ = alpStatus.lastHbByte1;
    }
}

void V1Display::drawObdIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    const bool wantShow = obdEnabled_;
    const bool curConnected = wantShow && obdConnected_;
    const bool curAttention = wantShow && !curConnected && (obdScanAttention_ || obdAttention_);

    if (!dirty_.obdIndicator && elementCaches_.obd.valid && wantShow == elementCaches_.obd.lastShown &&
        curConnected == elementCaches_.obd.lastConnected && curAttention == elementCaches_.obd.lastAttention) {
        return;
    }
    dirty_.obdIndicator = false; // Still cleared here — it's also read externally for flush
    elementCaches_.obd.valid = true;
    elementCaches_.obd.lastShown = wantShow;
    elementCaches_.obd.lastConnected = curConnected;
    elementCaches_.obd.lastAttention = curAttention;

    const DisplayLayout::DisplayRect badgeRect = DisplayLayout::obdBadgeRect();

    drawnRegion_.add(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h);
    FILL_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, PALETTE_BG);
    if (!wantShow) {
        return;
    }

    const V1Settings& s = settings_.get();
    const uint16_t textColor = curConnected ? s.colorObd : (curAttention ? 0xF800 : s.colorMuted);

    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(textColor, PALETTE_BG);
    GFX_drawString(tft_, "OBD", badgeRect.x + badgeRect.w / 2, badgeRect.y + badgeRect.h / 2);
#endif
}

void V1Display::drawGpsIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    const GpsRuntimeStatus gs = gpsRtMod_ ? gpsRtMod_->snapshot(millis()) : GpsRuntimeStatus{};
    const bool wantShow = gs.enabled && gs.stableHasFix;
    const uint8_t sats = wantShow ? gs.stableSatellites : 0;

    if (!dirty_.gpsIndicator && elementCaches_.gps.valid && wantShow == elementCaches_.gps.lastShown &&
        sats == elementCaches_.gps.lastSats) {
        return;
    }
    dirty_.gpsIndicator = false;
    elementCaches_.gps.valid = true;
    elementCaches_.gps.lastShown = wantShow;
    elementCaches_.gps.lastSats = sats;

    const DisplayLayout::DisplayRect badgeRect = DisplayLayout::gpsBadgeRect();

    drawnRegion_.add(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h);
    FILL_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, PALETTE_BG);
    if (!wantShow) {
        return;
    }

    char buf[6];
    snprintf(buf, sizeof(buf), "G%u", sats);
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(0x07E0, PALETTE_BG); // Green — stable GPS fix
    GFX_drawString(tft_, buf, badgeRect.x + badgeRect.w / 2, badgeRect.y + badgeRect.h / 2);
#endif
}

void V1Display::setAlpRuntimeModule(AlpRuntimeModule* m) {
    alpRtMod_ = m;
}

void V1Display::drawAlpIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    const bool wantShow = alpEnabled_;

    if (!dirty_.alpIndicator && elementCaches_.alp.valid && wantShow == elementCaches_.alp.lastShown &&
        alpStateRaw_ == elementCaches_.alp.lastState && alpHbByte1_ == elementCaches_.alp.lastHbByte1 &&
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

    drawnRegion_.add(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h);
    FILL_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, PALETTE_BG);
    if (!wantShow) {
        return;
    }

    // ALP pad convention: grey when off/idle, red only during a live laser event.
    // Otherwise B0 byte1 selects LID=04 (blue), DLI=03 (orange), or warm-up=02/00
    // (green).
    const V1Settings& s = settings_.get();
    const AlpState alpState = static_cast<AlpState>(alpStateRaw_);
    uint16_t textColor;
    if (alpState == AlpState::OFF || alpState == AlpState::IDLE) {
        textColor = s.colorMuted; // Grey — not connected
    } else if (alpHasLaserEvent_) {
        textColor = s.colorAlpAlert; // Red — active laser alert
    } else {
        switch (alpHbByte1_) {
        case 0x04:
            textColor = s.colorAlpLidActive; // Blue — LID active (above LID speed limit)
            break;
        case 0x03:
            textColor = s.colorAlpDli; // Orange — DLI active (below LID speed limit)
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
    GFX_drawString(tft_, "ALP", badgeRect.x + badgeRect.w / 2, badgeRect.y + badgeRect.h / 2);
#endif
}

void V1Display::setPreviewIndicatorOverridesActive(bool active) {
    if (previewIndicatorOverridesActive_ == active) {
        return;
    }

    previewIndicatorOverridesActive_ = active;
    // Changing preview ownership must repaint the previous owner's badges.
    dirty_.obdIndicator = true;
    dirty_.alpIndicator = true;
    elementCaches_.obd.invalidate();
    elementCaches_.alp.invalidate();
}

void V1Display::setAlpPreviewState(bool enabled, uint8_t state, uint8_t hbByte1) {
    const AlpState previewState = static_cast<AlpState>(state);
    const bool nextHasLaserEvent = (previewState == AlpState::ALERT_ACTIVE || previewState == AlpState::NOISE_WINDOW ||
                                    previewState == AlpState::TEARDOWN);

    if (alpEnabled_ == enabled && alpStateRaw_ == state && alpHbByte1_ == hbByte1 &&
        alpHasLaserEvent_ == nextHasLaserEvent && alpLaserEvent_.active == nextHasLaserEvent &&
        alpLaserEvent_.gun == AlpGunType::UNKNOWN && alpLaserEvent_.direction == AlpLaserDirection::UNKNOWN &&
        !alpLaserEvent_.lidActive) {
        return;
    }

    alpEnabled_ = enabled;
    alpStateRaw_ = state;
    alpHbByte1_ = hbByte1;
    // Preview synthesizes a live event from these states instead of runtime ownership.
    alpHasLaserEvent_ = nextHasLaserEvent;
    alpLaserEvent_.active = alpHasLaserEvent_;
    alpLaserEvent_.gun = AlpGunType::UNKNOWN;
    alpLaserEvent_.direction = AlpLaserDirection::UNKNOWN;
    alpLaserEvent_.lidActive = false;
    dirty_.alpIndicator = true;
    elementCaches_.alp.invalidate();
}

void V1Display::setObdPreviewState(bool enabled, bool connected, bool scanAttention) {
    if (obdEnabled_ == enabled && obdConnected_ == connected && obdScanAttention_ == scanAttention) {
        return;
    }

    setObdStatus(enabled, connected, scanAttention);
    dirty_.obdIndicator = true;
    elementCaches_.obd.invalidate();
}

void V1Display::setAlpLaserEvent(const AlpLaserEvent& ev) {
    const bool prevActive = alpHasLaserEvent_;
    const bool prevLidActive = alpLidActive_;
    const uint8_t prevDirection = static_cast<uint8_t>(alpLaserEvent_.direction);
    const bool prevFreqOverride = alpFreqOverride_;
    const AlpGunType prevGun = alpLaserEvent_.gun;
    // A new engagement can arrive without an inactive display frame between it
    // and the previous teardown. Retain identity only within the same session.
    const bool sameSession = ev.sessionGeneration != 0 && ev.sessionGeneration == alpLaserEvent_.sessionGeneration;
    const bool holdLiveGun = sameSession && prevActive && ev.active && ev.gun == AlpGunType::UNKNOWN &&
                             prevGun != AlpGunType::UNKNOWN;
    const AlpGunType effectiveGun = holdLiveGun ? prevGun : ev.gun;
    // Persisted tails may retain gun text without live badge/arrow colors.
    const bool nextFreqOverride = (effectiveGun != AlpGunType::UNKNOWN);
    const char* nextGunAbbr = nextFreqOverride ? alpGunAbbrev(effectiveGun) : "";
    const bool nextLidActive = nextFreqOverride ? ev.lidActive : false;
    const bool gunChanged = (prevFreqOverride != nextFreqOverride) ||
                            (nextFreqOverride && strncmp(alpFreqText_, nextGunAbbr, sizeof(alpFreqText_)) != 0);
    const bool visualChanged = (ev.active != prevActive) || (static_cast<uint8_t>(ev.direction) != prevDirection) ||
                               gunChanged || (nextLidActive != prevLidActive);

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

    if (visualChanged) {
        elementCaches_.alp.invalidate();
        elementCaches_.arrow.invalidate();
        elementCaches_.frequency.invalidate();
    }

}

void V1Display::setAlpFrequencyOverride(const char* gunAbbrev, bool lidActive) {
    if (!gunAbbrev) {
        clearAlpFrequencyOverride();
        return;
    }
    alpFreqOverride_ = true;
    alpLidActive_ = lidActive;
    strncpy(alpFreqText_, gunAbbrev, sizeof(alpFreqText_));
    alpFreqText_[sizeof(alpFreqText_) - 1] = '\0';
}

void V1Display::clearAlpFrequencyOverride() {
    alpFreqOverride_ = false;
    alpLidActive_ = false;
    alpFreqText_[0] = '\0';
}

void V1Display::drawStatusText(const char* text, uint16_t color) {
    TFT_CALL(setTextColor)(color, PALETTE_BG);
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    GFX_drawString(tft_, text, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
}
