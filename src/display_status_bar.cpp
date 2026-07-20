/**
 * Status-bar / peripheral indicators — extracted from display.cpp (Phase 2N)
 *
 * Contains drawVolumeIndicator, drawRssiIndicator, drawProfileIndicator,
 * drawBatteryIndicator, drawBLEProxyIndicator, and drawWiFiIndicator.
 */

#include "display.h"
#include "display_layout.h"
#include "display_draw.h"
#include "display_element_caches.h"
#include "display_palette.h"
#include "display_text.h"
#include "display_segments.h" // DisplaySegments namespace (using-directive below)
#include "display_font_manager.h"
#include "settings.h"
#include "battery_manager.h"
#include "wifi_manager.h"
#include "perf_metrics.h"
#include <cmath>
#include <cstring>

using namespace DisplaySegments;

// File-scoped hysteresis state for battery indicator
static bool s_batteryShowOnUSB = true;

// ============================================================================
// Volume indicator
// ============================================================================

void V1Display::drawVolumeIndicator(uint8_t mainVol, uint8_t muteVol) {
    // Draw volume indicator below bogey counter: "5V  0M" format
    const V1Settings& s = settingsManager.get();
    const DisplayLayout::DisplayRect mainRect = DisplayLayout::volumeMainRect();
    const DisplayLayout::DisplayRect muteRect = DisplayLayout::volumeMuteRect();
    const DisplayLayout::DisplayRect clearRect = DisplayLayout::volumeIndicatorRect();

    // Cache short-circuit: skip the FILL_RECT+text render when values + colors
    // haven't changed. This is load-bearing for partial-flush region math in
    // update(priority) — without it, drawnRegion_ would expand to the volume
    // cell every frame by the steady-state status-strip refresh.
    if (elementCaches_.volume.valid && mainVol == elementCaches_.volume.lastMainVol &&
        muteVol == elementCaches_.volume.lastMuteVol && s.colorVolumeMain == elementCaches_.volume.lastMainColor &&
        s.colorVolumeMute == elementCaches_.volume.lastMuteColor) {
        return;
    }
    elementCaches_.volume.valid = true;
    elementCaches_.volume.lastMainVol = mainVol;
    elementCaches_.volume.lastMuteVol = muteVol;
    elementCaches_.volume.lastMainColor = s.colorVolumeMain;
    elementCaches_.volume.lastMuteColor = s.colorVolumeMute;
    perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Volume);
    perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::VolumeChange);

    // Clear the area first - only clear what we need, BLE icon is at y=98
    drawnRegion_.add(clearRect.x, clearRect.y, clearRect.w, clearRect.h, DisplayDirtyRegionSource::Status);
    FILL_RECT(clearRect.x, clearRect.y, clearRect.w, clearRect.h, PALETTE_BG);

    // Draw main volume in blue, mute volume in yellow (user-configurable colors)
    GFX_setTextDatum(TL_DATUM); // Top-left alignment
    TFT_CALL(setTextSize)(2);   // Size 2 = ~16px height

    // Draw main volume "5V" in main volume color
    char mainBuf[5]; // allow up to three digits plus suffix and null
    snprintf(mainBuf, sizeof(mainBuf), "%dV", mainVol);
    TFT_CALL(setTextColor)(s.colorVolumeMain, PALETTE_BG);
    GFX_drawString(tft_, mainBuf, mainRect.x, mainRect.y);

    // Draw mute volume "0M" in mute volume color, offset to the right
    char muteBuf[5]; // allow up to three digits plus suffix and null
    snprintf(muteBuf, sizeof(muteBuf), "%dM", muteVol);
    TFT_CALL(setTextColor)(s.colorVolumeMute, PALETTE_BG);
    GFX_drawString(tft_, muteBuf, muteRect.x, muteRect.y); // Aligned with RSSI number
}

// ============================================================================
// RSSI indicator
// ============================================================================

void V1Display::drawRssiIndicator(int rssi) {
    // Draw BLE RSSI below volume indicator
    // Shows V1 RSSI and app RSSI (if connected) stacked vertically
    const DisplayLayout::DisplayRect rssiRect = DisplayLayout::rssiRect();
    const int x = rssiRect.x;
    const int y = rssiRect.y;
    const int lineHeight = rssiRect.h / 2;

    // Check if RSSI indicator is hidden
    const V1Settings& s = settingsManager.get();
    const bool hidden = (s.hideRssiIndicator || s.hideVolumeIndicator);
    if (hidden) {
        // Cache short-circuit: skip the clear-FILL_RECT when we already
        // rendered the hidden (empty) state last time. First transition
        // into hidden state still paints the clear rect once.
        if (elementCaches_.rssi.valid && elementCaches_.rssi.lastHidden) {
            return;
        }
        perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Rssi);
        drawnRegion_.add(rssiRect.x, rssiRect.y, rssiRect.w, rssiRect.h, DisplayDirtyRegionSource::Status);
        FILL_RECT(rssiRect.x, rssiRect.y, rssiRect.w, rssiRect.h, PALETTE_BG);
        elementCaches_.rssi.valid = true;
        elementCaches_.rssi.lastHidden = true;
        elementCaches_.rssi.v1LineValid = false;
        elementCaches_.rssi.appLineValid = false;
        return; // Don't draw anything
    }

    if (!hasFreshBleContext(millis())) {
        return; // Keep last-drawn RSSI visible; don't clear on stale context
    }

    // Get both RSSIs
    const int v1Rssi = rssi;               // V1 RSSI passed in
    const int appRssi = bleCtx_.proxyRssi; // App RSSI

    // Helper: RSSI dBm → color-code bucket (identical to the logic below,
    // used here for cache-key comparison so color transitions force a redraw).
    auto rssiColorFor = [](int r) -> uint16_t {
        if (r >= -75)
            return COLOR_GREEN;
        if (r >= -90)
            return COLOR_YELLOW;
        return COLOR_RED;
    };
    const uint16_t v1Color = (v1Rssi != 0) ? rssiColorFor(v1Rssi) : 0;
    const uint16_t appColor = (appRssi != 0) ? rssiColorFor(appRssi) : 0;

    const bool forceBothLines = !elementCaches_.rssi.valid || elementCaches_.rssi.lastHidden;
    const bool v1Changed = forceBothLines || !elementCaches_.rssi.v1LineValid ||
                           v1Rssi != elementCaches_.rssi.lastV1Rssi || v1Color != elementCaches_.rssi.lastV1Color;
    const bool appChanged = forceBothLines || !elementCaches_.rssi.appLineValid ||
                            appRssi != elementCaches_.rssi.lastAppRssi || appColor != elementCaches_.rssi.lastAppColor;

    if (!v1Changed && !appChanged) {
        return;
    }

    elementCaches_.rssi.valid = true;
    elementCaches_.rssi.lastHidden = false;
    elementCaches_.rssi.lastV1Rssi = v1Rssi;
    elementCaches_.rssi.lastAppRssi = appRssi;
    elementCaches_.rssi.lastV1Color = v1Color;
    elementCaches_.rssi.lastAppColor = appColor;
    elementCaches_.rssi.v1LineValid = true;
    elementCaches_.rssi.appLineValid = true;
    perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Rssi);
    perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::RssiRefresh);

    auto clearLine = [&](int lineY) {
        drawnRegion_.add(static_cast<int16_t>(x), static_cast<int16_t>(lineY), rssiRect.w,
                         static_cast<uint16_t>(lineHeight), DisplayDirtyRegionSource::Status);
        FILL_RECT(x, lineY, rssiRect.w, lineHeight, PALETTE_BG);
    };
    const bool redrawBothLines = v1Changed && appChanged;
    if (redrawBothLines) {
        drawnRegion_.add(rssiRect.x, rssiRect.y, rssiRect.w, rssiRect.h, DisplayDirtyRegionSource::Status);
        FILL_RECT(rssiRect.x, rssiRect.y, rssiRect.w, rssiRect.h, PALETTE_BG);
    } else {
        if (v1Changed) {
            clearLine(y);
        }
        if (appChanged) {
            clearLine(y + lineHeight);
        }
    }

    GFX_setTextDatum(TL_DATUM);
    TFT_CALL(setTextSize)(2); // Match volume text size

    // Draw V1 RSSI if connected
    if ((redrawBothLines || v1Changed) && v1Rssi != 0) {
        // Draw "V " label with configurable color
        TFT_CALL(setTextColor)(s.colorRssiV1, PALETTE_BG);
        GFX_drawString(tft_, "V ", x, y);

        TFT_CALL(setTextColor)(v1Color, PALETTE_BG);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", v1Rssi);
        GFX_drawString(tft_, buf, x + 24, y); // Offset for "V " width
    }

    // Draw app RSSI below V1 RSSI if connected
    if ((redrawBothLines || appChanged) && appRssi != 0) {
        // Draw "P " label with configurable color
        TFT_CALL(setTextColor)(s.colorRssiProxy, PALETTE_BG);
        GFX_drawString(tft_, "P ", x, y + lineHeight);

        TFT_CALL(setTextColor)(appColor, PALETTE_BG);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", appRssi);
        GFX_drawString(tft_, buf, x + 24, y + lineHeight); // Offset for "P " width
    }
}

// ============================================================================
// Profile indicator
// ============================================================================

void V1Display::setProfileIndicatorSlot(int slot) {
    if (slot != lastProfileSlot_) {
        lastProfileSlot_ = slot;
        profileChangedTime_ = static_cast<uint32_t>(millis());
    }
    currentProfileSlot_ = slot;
}

void V1Display::drawProfileIndicator(int slot) {
    // Get custom slot names and colors from settings
    const V1Settings& s = settingsManager.get();

    setProfileIndicatorSlot(slot);

    // Check if we're in the "flash" period after a profile change
    const uint32_t nowMs = static_cast<uint32_t>(millis());
    bool inFlashPeriod = (nowMs - profileChangedTime_) < HIDE_TIMEOUT_MS;

#if defined(DISPLAY_WAVESHARE_349)
    // On Waveshare: draw profile indicator under arrows (for autopush profiles)
    const DisplayLayout::DisplayRect profileRect = DisplayLayout::profileRect();
    const int cx = profileRect.x + profileRect.w / 2;

    // The profile FILL_RECT covers x∈[cx-65, cx+65) = [499, 629), y∈[152, 172)
    // which clips the bottom-left 11×12 px of the battery body DRAW_RECT at
    // [618,632) × [136,164). Any path that FILL_RECTs here must invalidate
    // the battery icon cache and call drawBatteryIndicator() to restore the
    // clipped corner. The cache short-circuit below ensures both of those
    // only happen on frames where the profile state actually changed.

    // If user explicitly hides the indicator via web UI, only show during flash period.
    // Hidden-state render is just a FILL_RECT clear; use the cache to skip repaints.
    if (s.hideProfileIndicator && !inFlashPeriod) {
        if (elementCaches_.profile.valid && elementCaches_.profile.hiddenRender) {
            return;
        }
        perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Profile);
        drawnRegion_.add(profileRect.x, profileRect.y, profileRect.w, profileRect.h, DisplayDirtyRegionSource::Status);
        FILL_RECT(profileRect.x, profileRect.y, profileRect.w, profileRect.h, PALETTE_BG);
        elementCaches_.profile.valid = true;
        elementCaches_.profile.hiddenRender = true;
        elementCaches_.profile.lastName[0] = '\0';
        elementCaches_.profile.lastSlot = -1;
        // Repaint the battery icon whose bottom-left corner we just wiped.
        // (WiFi icon is at x=[6,30], y=[143,167] — does not overlap the
        //  profile FILL_RECT at x=[499,629), y=[152,172), so no WiFi repaint
        //  needed. drawProfileIndicator is scoped to the one indicator it
        //  actually clips.)
        elementCaches_.battery.iconValid = false;
        drawBatteryIndicator();
        return;
    }

    // Use custom names, fallback to defaults (limited to 20 chars)
    const char* name;
    uint16_t color;
    switch (slot % 3) {
    case 0:
        name = s.slot0Name.length() > 0 ? s.slot0Name.c_str() : "DEFAULT";
        color = s.slot0Color;
        break;
    case 1:
        name = s.slot1Name.length() > 0 ? s.slot1Name.c_str() : "HIGHWAY";
        color = s.slot1Color;
        break;
    default: // case 2
        name = s.slot2Name.length() > 0 ? s.slot2Name.c_str() : "COMFORT";
        color = s.slot2Color;
        break;
    }

    // Cache short-circuit: skip FILL_RECT+text render when slot/color/name
    // match the last-drawn values. This is what makes the arrow-only partial
    // flush actually fire — profile is called every frame from drawStatusStrip.
    if (elementCaches_.profile.valid && !elementCaches_.profile.hiddenRender &&
        slot == elementCaches_.profile.lastSlot && color == elementCaches_.profile.lastColor &&
        strncmp(name, elementCaches_.profile.lastName, sizeof(elementCaches_.profile.lastName)) == 0) {
        return;
    }
    elementCaches_.profile.valid = true;
    elementCaches_.profile.hiddenRender = false;
    elementCaches_.profile.lastSlot = slot;
    elementCaches_.profile.lastColor = color;
    strncpy(elementCaches_.profile.lastName, name, sizeof(elementCaches_.profile.lastName) - 1);
    elementCaches_.profile.lastName[sizeof(elementCaches_.profile.lastName) - 1] = '\0';
    perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Profile);

    // Clear area under arrows
    drawnRegion_.add(profileRect.x, profileRect.y, profileRect.w, profileRect.h, DisplayDirtyRegionSource::Status);
    FILL_RECT(profileRect.x, profileRect.y, profileRect.w, profileRect.h, PALETTE_BG);

    // Use built-in font (OFR font subset doesn't have all letters for profile names)
    TFT_CALL(setTextSize)(2); // Size 2 = ~12px per char
    TFT_CALL(setTextColor)(color, PALETTE_BG);
    int16_t nameWidth = strlen(name) * 12; // size 2 = ~12px per char
    int textX = cx - nameWidth / 2;
    GFX_setTextDatum(TL_DATUM);
    GFX_drawString(tft_, name, textX, profileRect.y);

    // Profile FILL_RECT clipped the battery body's bottom-left corner.
    // Invalidate its icon cache + repaint so the clipped corner is restored.
    // Cache short-circuit in drawBatteryIndicator keeps this cheap on frames
    // where profile didn't redraw (this path is reached only on profile change).
    elementCaches_.battery.iconValid = false;
    drawBatteryIndicator();
#endif
}

// ============================================================================
// Battery indicator
// ============================================================================

void V1Display::drawBatteryIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    // Nothing may be painted, cached, or marked dirty until the source
    // classifier has a real verdict. Before that, isOnBattery() reports the
    // safe default rather than an observation; rendering it produces a visibly
    // wrong indicator that must then be repainted when the verdict lands, and
    // that repaint drags a full-flush redraw with it. Returning here leaves the
    // element caches untouched, so the eventual first paint is an ordinary
    // first paint rather than a transition. See bug #17 and
    // battery_source_policy::batteryIndicatorShouldPaint.
    if (!batteryManager.batteryIndicatorReady()) {
        return;
    }

    const V1Settings& s = settingsManager.get();

    // Battery icon position - VERTICAL at bottom-right
    // Position to the right of profile indicator area, avoiding direction arrows
    const int battW = 14;                        // Battery body width (was height when horizontal)
    const int battH = 28;                        // Battery body height (was width when horizontal)
    const int battX = SCREEN_WIDTH - battW - 8;  // Right edge with margin
    const int battY = SCREEN_HEIGHT - battH - 8; // Bottom with margin (cap above)
    const int capW = 8;                          // Positive terminal cap width (horizontal bar at top)
    const int capH = 3;                          // Positive terminal cap height
    const DisplayLayout::DisplayRect percentRect = DisplayLayout::batteryPercentRect();
    const DisplayLayout::DisplayRect iconRect = DisplayLayout::batteryIconRect();

    // Hide battery when on USB power (voltage near max)
    // Use hysteresis to prevent flickering: hide above 4125, show below 4095
    uint16_t voltage = batteryManager.getVoltageMillivolts();
    if (voltage > 4125) {
        s_batteryShowOnUSB = false; // On USB or fully charged
    } else if (voltage < 4095) {
        s_batteryShowOnUSB = true; // On battery, not full
    }
    // Between 4095-4125: keep previous state (hysteresis)

    // Get battery percentage for display
    uint8_t pct = batteryManager.getPercentage();

    // If percent is enabled, ONLY show percent (never icon)
    if (s.showBatteryPercent && !s.hideBatteryIcon && batteryManager.hasBattery()) {
        const unsigned long PCT_FORCE_REDRAW_MS = 60000; // 60s safety refresh

        // Choose color based on level
        uint16_t textColor;
        if (pct <= 20) {
            textColor = 0xF800; // Red - critical
        } else if (pct <= 40) {
            textColor = 0xFD20; // Orange - low
        } else {
            textColor = 0x07E0; // Green - good
        }
        textColor = dimColor(textColor);

        // Decide if we actually need to redraw — check BEFORE any FILL_RECT
        // so the cache short-circuit fires cleanly on steady-state frames.
        const unsigned long nowMs = millis();
        const bool needsRedraw = (!elementCaches_.battery.lastPctVisible) ||
                                 (pct != elementCaches_.battery.lastPctDrawn) ||
                                 (textColor != elementCaches_.battery.lastPctColor) ||
                                 ((nowMs - elementCaches_.battery.lastPctDrawMs) >= PCT_FORCE_REDRAW_MS) ||
                                 !elementCaches_.battery.iconValid; // mode transition

        // Mode just transitioned from icon → percent, or first render.
        // Clear the icon area once; then cache state tracks percent-mode going forward.
        if (!elementCaches_.battery.iconValid) {
            perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Battery);
            drawnRegion_.add(iconRect.x, iconRect.y, iconRect.w, iconRect.h, DisplayDirtyRegionSource::Status);
            FILL_RECT(iconRect.x, iconRect.y, iconRect.w, iconRect.h, PALETTE_BG);
            elementCaches_.battery.iconValid = true;
            elementCaches_.battery.lastIconShown = false;
            elementCaches_.battery.lastFilledSections = -1;
        }

        // Only draw percent if not on USB
        if (!s_batteryShowOnUSB) {
            // Clear percent area when not visible
            if (elementCaches_.battery.lastPctVisible) {
                perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Battery);
                drawnRegion_.add(percentRect.x, percentRect.y, percentRect.w, percentRect.h,
                                 DisplayDirtyRegionSource::Status);
                FILL_RECT(percentRect.x, percentRect.y, percentRect.w, percentRect.h, PALETTE_BG);
                elementCaches_.battery.lastPctVisible = false;
                elementCaches_.battery.lastPctDrawn = -1;
            }
            return; // No percent on USB/fully charged
        }

        if (!needsRedraw) {
            return; // Skip expensive render when nothing changed
        }
        perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Battery);
        drawnRegion_.add(percentRect.x, percentRect.y, percentRect.w, percentRect.h, DisplayDirtyRegionSource::Status);

        // Format percentage string (no % to save space)
        char pctStr[4];
        snprintf(pctStr, sizeof(pctStr), "%d", pct);

        // Always clear the text area before drawing (covers shorter numbers)
        // Positioned to avoid top arrow which extends to roughly SCREEN_WIDTH - 14
        FILL_RECT(percentRect.x, percentRect.y, percentRect.w, percentRect.h, PALETTE_BG);

        // Right-aligned built-in font near the top-right corner.
        GFX_setTextDatum(TR_DATUM);
        TFT_CALL(setTextSize)(2); // Larger for better visibility
        TFT_CALL(setTextColor)(textColor, PALETTE_BG);
        GFX_drawString(tft_, pctStr, percentRect.x + percentRect.w - 2, percentRect.y + 12);

        // Update cache
        elementCaches_.battery.lastPctDrawn = pct;
        elementCaches_.battery.lastPctColor = textColor;
        elementCaches_.battery.lastPctVisible = true;
        elementCaches_.battery.lastPctDrawMs = nowMs;
        return; // Never draw icon when percent is enabled
    }

    // Percent is disabled, show icon instead.
    // Compute icon state up front so we can cache-compare before touching pixels.
    const bool hasBat = batteryManager.hasBattery();
    const bool hideIcon = s.hideBatteryIcon;
    const bool shownNow = hasBat && !hideIcon && s_batteryShowOnUSB;

    int filledSections = 0;
    uint16_t fillColor = 0x07E0; // default green
    if (shownNow) {
        const int sections = 5;
        filledSections = (pct + 10) / 20; // 0-20%=1, 21-40%=2, etc. (min 1 if >0)
        if (pct == 0)
            filledSections = 0;
        if (filledSections > sections)
            filledSections = sections;

        if (pct <= 20)
            fillColor = 0xF800; // Red - critical
        else if (pct <= 40)
            fillColor = 0xFD20; // Orange - low
        else
            fillColor = 0x07E0; // Green - good
    }
    const uint16_t outlineColor = dimColor(PALETTE_TEXT);

    // Cache short-circuit: skip repaint when icon-mode state unchanged.
    // iconValid=false forces a repaint (used by drawProfileIndicator after
    // its FILL_RECT clips the battery corner, and by invalidateAll()).
    if (elementCaches_.battery.iconValid && elementCaches_.battery.lastIconShown == shownNow &&
        elementCaches_.battery.lastFilledSections == filledSections &&
        elementCaches_.battery.lastFillColor == fillColor && elementCaches_.battery.lastOutlineColor == outlineColor &&
        elementCaches_.battery.lastHasBattery == hasBat && elementCaches_.battery.lastHideIcon == hideIcon &&
        elementCaches_.battery.lastOnUSB == s_batteryShowOnUSB &&
        elementCaches_.battery.lastShowPercent == s.showBatteryPercent) {
        return;
    }
    // Clear percent area (in case it was previously showing, or mode transition)
    if (elementCaches_.battery.lastPctVisible || !elementCaches_.battery.iconValid) {
        perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Battery);
        drawnRegion_.add(percentRect.x, percentRect.y, percentRect.w, percentRect.h, DisplayDirtyRegionSource::Status);
        FILL_RECT(percentRect.x, percentRect.y, percentRect.w, percentRect.h, PALETTE_BG);
        elementCaches_.battery.lastPctVisible = false;
        elementCaches_.battery.lastPctDrawn = -1;
    }

    // Don't draw icon if no battery, user hides it, or on USB
    if (!shownNow) {
        perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Battery);
        drawnRegion_.add(iconRect.x, iconRect.y, iconRect.w, iconRect.h, DisplayDirtyRegionSource::Status);
        FILL_RECT(iconRect.x, iconRect.y, iconRect.w, iconRect.h, PALETTE_BG);
        elementCaches_.battery.iconValid = true;
        elementCaches_.battery.lastIconShown = false;
        elementCaches_.battery.lastFilledSections = 0;
        elementCaches_.battery.lastFillColor = fillColor;
        elementCaches_.battery.lastOutlineColor = outlineColor;
        elementCaches_.battery.lastHasBattery = hasBat;
        elementCaches_.battery.lastHideIcon = hideIcon;
        elementCaches_.battery.lastOnUSB = s_batteryShowOnUSB;
        elementCaches_.battery.lastShowPercent = s.showBatteryPercent;
        return;
    }

    const int padding = 2;  // Padding inside battery
    const int sections = 5; // Number of charge sections

    // Clear area (including cap above)
    perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Battery);
    drawnRegion_.add(iconRect.x, iconRect.y, iconRect.w, iconRect.h, DisplayDirtyRegionSource::Status);
    FILL_RECT(iconRect.x, iconRect.y, iconRect.w, iconRect.h, PALETTE_BG);

    // Draw battery outline (dimmed) - vertical orientation
    DRAW_RECT(battX, battY, battW, battH, outlineColor); // Main body
    // Positive cap at top, centered
    FILL_RECT(battX + (battW - capW) / 2, battY - capH, capW, capH, outlineColor);

    // Draw charge sections (vertical - bottom to top, filled from bottom)
    int sectionH = (battH - 2 * padding - (sections - 1)) / sections; // Height of each section with 1px gap
    for (int i = 0; i < sections; i++) {
        // Draw from bottom up: section 0 at bottom, section 4 at top
        int sx = battX + padding;
        int sy = battY + battH - padding - (i + 1) * sectionH - i; // Bottom-up
        int sw = battW - 2 * padding;

        if (i < filledSections) {
            FILL_RECT(sx, sy, sw, sectionH, dimColor(fillColor));
        }
    }

    // Commit icon cache
    elementCaches_.battery.iconValid = true;
    elementCaches_.battery.lastIconShown = true;
    elementCaches_.battery.lastFilledSections = filledSections;
    elementCaches_.battery.lastFillColor = fillColor;
    elementCaches_.battery.lastOutlineColor = outlineColor;
    elementCaches_.battery.lastHasBattery = hasBat;
    elementCaches_.battery.lastHideIcon = hideIcon;
    elementCaches_.battery.lastOnUSB = s_batteryShowOnUSB;
    elementCaches_.battery.lastShowPercent = s.showBatteryPercent;
#endif
}

// ============================================================================
// BLE proxy indicator
// ============================================================================

void V1Display::drawBLEProxyIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    const DisplayLayout::DisplayRect badgeRect = DisplayLayout::bleBadgeRect();
    const DisplayLayout::DisplayRect iconRect = DisplayLayout::bleIconRect();
    const int iconSize = iconRect.w;
    const int bleX = iconRect.x;
    const int bleY = iconRect.y;

    if (!bleProxyEnabled_) {
        if (bleProxyDrawn_) {
            perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::BleProxy);
            drawnRegion_.add(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, DisplayDirtyRegionSource::Status);
            FILL_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, PALETTE_BG);
            bleProxyDrawn_ = false;
        }
        // Cache-short-circuit: if we already rendered the cleared state, return.
        if (elementCaches_.bleProxy.valid && !elementCaches_.bleProxy.lastDrawn) {
            return;
        }
        elementCaches_.bleProxy.valid = true;
        elementCaches_.bleProxy.lastDrawn = false;
        return;
    }

    // Check if BLE icon should be hidden
    const V1Settings& s = settingsManager.get();
    if (s.hideBleIcon) {
        if (bleProxyDrawn_) {
            perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::BleProxy);
            drawnRegion_.add(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, DisplayDirtyRegionSource::Status);
            FILL_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, PALETTE_BG);
            bleProxyDrawn_ = false;
        }
        if (elementCaches_.bleProxy.valid && !elementCaches_.bleProxy.lastDrawn) {
            return;
        }
        elementCaches_.bleProxy.valid = true;
        elementCaches_.bleProxy.lastDrawn = false;
        return;
    }

    // Visual-verification preview steps own their BLE state explicitly.  Do not
    // let an unrelated runtime-context timestamp make the same pinned step
    // alternate between the bright and stale connected roles across captures.
    const bool bleContextFresh = previewIndicatorOverridesActive_ || hasFreshBleContext(millis());

    // Icon color from settings: connected vs disconnected
    // When connected but not receiving data, dim further to show "stale" state
    uint16_t btColor;
    if (bleProxyClientConnected_) {
        // Connected: bright green when receiving, dimmed when stale
        const bool receivingData = bleReceivingData_ && bleContextFresh;
        btColor = receivingData ? dimColor(s.colorBleConnected, 85)
                                : dimColor(s.colorBleConnected, 40); // Much dimmer when no data
    } else {
        btColor = dimColor(s.colorBleDisconnected, 85);
    }

    // Cache short-circuit: skip rune rebuild when already drawn with same color.
    // bleProxyDrawn_ is an older singleton latch kept in sync here so existing
    // consumers (e.g. prepareFullRedrawNoClear) still observe it correctly.
    if (elementCaches_.bleProxy.valid && elementCaches_.bleProxy.lastDrawn &&
        elementCaches_.bleProxy.lastColor == btColor && bleProxyDrawn_) {
        return;
    }
    elementCaches_.bleProxy.valid = true;
    elementCaches_.bleProxy.lastDrawn = true;
    elementCaches_.bleProxy.lastColor = btColor;

    // Clear the area before redrawing
    perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::BleProxy);
    drawnRegion_.add(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, DisplayDirtyRegionSource::Status);
    FILL_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, PALETTE_BG);

    // Draw Bluetooth rune - the bind rune of ᛒ (Berkanan) and ᚼ (Hagall)
    // Center point of the icon
    int cx = bleX + iconSize / 2;
    int cy = bleY + iconSize / 2;

    int h = iconSize - 2; // Total height
    int top = cy - h / 2;
    int bot = cy + h / 2;
    int mid = cy;

    // Right chevron points - where the arrows reach on the right
    int rightX = cx + 5;
    int topChevronY = mid - 4; // Upper right point
    int botChevronY = mid + 4; // Lower right point

    // Left arrow endpoints
    int leftX = cx - 5;
    int topArrowY = mid - 4; // Upper left point
    int botArrowY = mid + 4; // Lower left point

    // Vertical center line (thicker for visibility)
    FILL_RECT(cx - 1, top, 2, h, btColor);

    // ============================================================================
    // RIGHT SIDE: Two chevrons forming the "B"
    // ============================================================================
    // Top chevron: top of line → right point → center (draw 3 lines for thickness)
    DRAW_LINE(cx - 1, top, rightX - 1, topChevronY, btColor);
    DRAW_LINE(cx, top, rightX, topChevronY, btColor);
    DRAW_LINE(cx + 1, top, rightX + 1, topChevronY, btColor);
    DRAW_LINE(rightX - 1, topChevronY, cx - 1, mid, btColor);
    DRAW_LINE(rightX, topChevronY, cx, mid, btColor);
    DRAW_LINE(rightX + 1, topChevronY, cx + 1, mid, btColor);

    // Bottom chevron: center → right point → bottom of line (draw 3 lines for thickness)
    DRAW_LINE(cx - 1, mid, rightX - 1, botChevronY, btColor);
    DRAW_LINE(cx, mid, rightX, botChevronY, btColor);
    DRAW_LINE(cx + 1, mid, rightX + 1, botChevronY, btColor);
    DRAW_LINE(rightX - 1, botChevronY, cx - 1, bot, btColor);
    DRAW_LINE(rightX, botChevronY, cx, bot, btColor);
    DRAW_LINE(rightX + 1, botChevronY, cx + 1, bot, btColor);

    // ============================================================================
    // LEFT SIDE: Two arrows forming the "X" through center
    // ============================================================================
    // Upper-left arrow (draw 3 lines for thickness)
    DRAW_LINE(leftX - 1, topArrowY, cx - 1, mid, btColor);
    DRAW_LINE(leftX, topArrowY, cx, mid, btColor);
    DRAW_LINE(leftX + 1, topArrowY, cx + 1, mid, btColor);

    // Lower-left arrow (draw 3 lines for thickness)
    DRAW_LINE(leftX - 1, botArrowY, cx - 1, mid, btColor);
    DRAW_LINE(leftX, botArrowY, cx, mid, btColor);
    DRAW_LINE(leftX + 1, botArrowY, cx + 1, mid, btColor);

    bleProxyDrawn_ = true;
#endif
}

// ============================================================================
// WiFi indicator
// ============================================================================

void V1Display::drawWiFiIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    const V1Settings& s = settingsManager.get();

    const DisplayLayout::DisplayRect badgeRect = DisplayLayout::wifiBadgeRect();
    const DisplayLayout::DisplayRect iconRect = DisplayLayout::wifiIconRect();
    const int wifiX = iconRect.x;
    const int wifiY = iconRect.y;
    const int wifiSize = iconRect.w;

    // Check if user explicitly hides the WiFi icon
    if (s.hideWifiIcon) {
        // Cache short-circuit: skip clear FILL_RECT when already rendered as hidden
        if (elementCaches_.wifi.valid && !elementCaches_.wifi.lastVisible) {
            return;
        }
        perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Wifi);
        drawnRegion_.add(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, DisplayDirtyRegionSource::Status);
        FILL_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, PALETTE_BG);
        elementCaches_.wifi.valid = true;
        elementCaches_.wifi.lastVisible = false;
        return;
    }

    const bool wifiServiceActive = wifiManager.isWifiServiceActive();
    const bool staConnected = wifiManager.isConnected();
    const bool showWifiIcon = wifiServiceActive || staConnected;

    if (!showWifiIcon) {
        // Cache short-circuit: skip clear FILL_RECT when already rendered as hidden
        if (elementCaches_.wifi.valid && !elementCaches_.wifi.lastVisible) {
            return;
        }
        perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Wifi);
        drawnRegion_.add(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, DisplayDirtyRegionSource::Status);
        // Clear the WiFi icon area when WiFi is fully inactive.
        FILL_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, PALETTE_BG);
        elementCaches_.wifi.valid = true;
        elementCaches_.wifi.lastVisible = false;
        return;
    }

    // WiFi icon color: gave-up (red) > operational (green).
    // If the icon is visible at all, WiFi is running — show green.
    // Red only when STA reconnect has exhausted all retries.
    const bool gaveUp = wifiManager.isReconnectGaveUp();
    uint16_t wifiColor;
    if (gaveUp && !staConnected) {
        wifiColor = 0xF800; // Bright red — STA gave up after max reconnect failures
    } else {
        wifiColor = dimColor(s.colorWiFiConnected, 85);
    }

    // Cache short-circuit: skip arc geometry rebuild when already drawn with same color.
    if (elementCaches_.wifi.valid && elementCaches_.wifi.lastVisible && elementCaches_.wifi.lastColor == wifiColor) {
        return;
    }
    perfRecordDisplayStatusPaint(PerfDisplayStatusPaint::Wifi);
    drawnRegion_.add(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, DisplayDirtyRegionSource::Status);
    elementCaches_.wifi.valid = true;
    elementCaches_.wifi.lastVisible = true;
    elementCaches_.wifi.lastColor = wifiColor;

    // Clear area first
    FILL_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, PALETTE_BG);

    // Center point for arcs (bottom center of icon area)
    int cx = wifiX + wifiSize / 2;
    int cy = wifiY + wifiSize - 3;

    // Draw center dot (the WiFi source point)
    FILL_RECT(cx - 2, cy - 2, 5, 5, wifiColor);

    // Draw 3 concentric arcs above the dot.
    //
    // Geometry is device-constant (fixed radii 5/9/13 and fixed angle step
    // tables), so we precompute (dx, dy) offsets once on first call and reuse
    // them forever.  This removes ~28 double-precision sin/cos evaluations
    // from every redraw path (full-redraw or indicator-color flip) and leaves
    // only integer adds + FILL_RECT calls.  Bit-identical to the previous
    // truncation math: kPi and casts match the original expression exactly.
    struct ArcPoint {
        int8_t dx;
        int8_t dy;
    };
    static const struct ArcPoints {
        ArcPoint arc1[7];  // inner: -45..45 step 15, r=5
        ArcPoint arc2[9];  // middle: -50..50 step 12, r=9
        ArcPoint arc3[12]; // outer: -55..55 step 10, r=13
    } kArcPoints = [] {
        constexpr double kPi = 3.14159;
        ArcPoints pts{};
        int idx = 0;
        for (int angle = -45; angle <= 45; angle += 15) {
            const double rad = angle * kPi / 180.0;
            pts.arc1[idx].dx = static_cast<int8_t>(static_cast<int>(5 * sin(rad)));
            pts.arc1[idx].dy = static_cast<int8_t>(-5 - static_cast<int>(5 * cos(rad)));
            ++idx;
        }
        idx = 0;
        for (int angle = -50; angle <= 50; angle += 12) {
            const double rad = angle * kPi / 180.0;
            pts.arc2[idx].dx = static_cast<int8_t>(static_cast<int>(9 * sin(rad)));
            pts.arc2[idx].dy = static_cast<int8_t>(-5 - static_cast<int>(9 * cos(rad)));
            ++idx;
        }
        idx = 0;
        for (int angle = -55; angle <= 55; angle += 10) {
            const double rad = angle * kPi / 180.0;
            pts.arc3[idx].dx = static_cast<int8_t>(static_cast<int>(13 * sin(rad)));
            pts.arc3[idx].dy = static_cast<int8_t>(-5 - static_cast<int>(13 * cos(rad)));
            ++idx;
        }
        return pts;
    }();

    for (const auto& p : kArcPoints.arc1) {
        FILL_RECT(cx + p.dx, cy + p.dy, 2, 2, wifiColor);
    }
    for (const auto& p : kArcPoints.arc2) {
        FILL_RECT(cx + p.dx, cy + p.dy, 2, 2, wifiColor);
    }
    for (const auto& p : kArcPoints.arc3) {
        FILL_RECT(cx + p.dx, cy + p.dy, 2, 2, wifiColor);
    }
#endif
}
