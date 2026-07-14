/**
 * Screen-mode renderers — extracted from display.cpp (Phase 3A)
 *
 * Contains showDisconnected, showMaintenanceMode, showResting, showScanning,
 * showBootSplash, showShutdown, showLowBattery, forceNextRedraw,
 * and resetChangeTracking.
 */

#include "display.h"
#include "config.h"
#include "display_layout.h"
#include "display_draw.h"
#include "display_dirty_flags.h"
#include "display_palette.h"
#include "display_text.h"
#include "display_segments.h"
#include "display_log.h"
#include "display_flush.h"
#include "display_font_manager.h"
#include "v1simple_logo.h"
#include "settings.h"
#include "perf_metrics.h"
#include <esp_heap_caps.h>

using namespace DisplaySegments;
using DisplayLayout::PRIMARY_ZONE_HEIGHT;

// ============================================================================
// showDisconnected
// ============================================================================

void V1Display::showDisconnected() {
    drawBaseFrame();
    drawStatusText("Disconnected", 0xF800); // Red
    drawWiFiIndicator();
    drawBatteryIndicator();
}

// ============================================================================
// showMaintenanceMode
// ============================================================================

void V1Display::showMaintenanceMode() {
    dirty_.multiAlert = true;
    multiAlertMode_ = false;
    persistedMode_ = false;
    drawnRegion_.reset();
    arrowVisibilityForceFullFlush_ = false;

    drawBaseFrame();

    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(3);
    TFT_CALL(setTextColor)(0x07FF, PALETTE_BG); // Cyan
    GFX_drawString(tft_, "MAINTENANCE", SCREEN_WIDTH / 2, 48);
    GFX_drawString(tft_, "MODE", SCREEN_WIDTH / 2, 82);

    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(PALETTE_TEXT, PALETTE_BG);
    GFX_drawString(tft_, "WiFi setup active", SCREEN_WIDTH / 2, 120);

    TFT_CALL(setTextSize)(1);
    TFT_CALL(setTextColor)(PALETTE_GRAY, PALETTE_BG);
    GFX_drawString(tft_, "Hold BOOT 4s to exit", SCREEN_WIDTH / 2, 150);

    drawWiFiIndicator();
    drawBatteryIndicator();

    lastState_ = DisplayState();
    if (currentScreen_ != ScreenMode::Maintenance) {
        perfRecordDisplayScreenTransition(perfScreenForMode(currentScreen_), PerfDisplayScreen::Unknown, millis());
    }
    currentScreen_ = ScreenMode::Maintenance;
    lastRestingProfileSlot_ = -1;

    DISPLAY_FLUSH();
}

// ============================================================================
// showResting
// ============================================================================

void V1Display::showResting(bool forceRedraw) {
    const PerfDisplayRenderScenario renderScenario = perfGetDisplayRenderScenario();
    const bool restoreRender = (renderScenario == PerfDisplayRenderScenario::Restore);
    unsigned long renderStartUs = 0;
    bool recordRenderTiming = false;
    // Always use multi-alert layout positioning
    dirty_.multiAlert = true;
    multiAlertMode_ = false;

    // Save the last known bogey counter before potentially resetting
    // This preserves the mode indicator (A/L/c) when V1 is connected.
    // Single-LED bogey counter — image1 only. FSD-002 Verdict Reversal:
    // image2 is the off-phase blink pair, not a second physical digit.
    char savedBogeyChar = lastState_.bogeyCounterChar;
    bool savedBogeyDot = lastState_.bogeyCounterDot;

    // Avoid redundant full-screen clears/flushes when already resting and nothing changed
    bool paletteChanged = (lastRestingPaletteRevision_ != paletteRevision_);
    bool screenChanged = (currentScreen_ != ScreenMode::Resting);
    int profileSlot = currentProfileSlot_;
    bool profileChanged = (profileSlot != lastRestingProfileSlot_);

    if (forceRedraw || screenChanged || paletteChanged) {
        perfRecordDisplayRenderPath(restoreRender ? PerfDisplayRenderPath::Restore
                                                  : PerfDisplayRenderPath::RestingFull);
        renderStartUs = micros();
        recordRenderTiming = true;
        // Full redraw when forced, coming from another screen, or after theme change
        drawBaseFrame();

        // Draw idle state: if V1 is connected, show last known mode; otherwise show "0"
        char topChar = '0';
        bool topDot = true;
        if (bleCtx_.v1Connected && savedBogeyChar != 0) {
            topChar = savedBogeyChar;
            topDot = savedBogeyDot;
        }
        drawTopCounterPair(topChar, false, topDot);
        // Volume indicator not shown in resting state (no DisplayState available)

        // Band indicators all dimmed (no active bands)
        drawBandIndicators(0, false);

        // Signal bars all empty
        drawVerticalSignalBars(0, 0, BAND_KA, false);

        // Direction arrows all dimmed
        drawDirectionArrow(DIR_NONE, false);

        // Frequency display
        drawFrequency(0, BAND_NONE);

        // Mute indicator off
        drawMuteIcon(false);
        syncTopIndicators(millis());
        drawObdIndicator();
        drawAlpIndicator();

        // Profile indicator
        drawProfileIndicator(profileSlot);

        // Reset secondary alert card state so stale live-alert cards can't
        // survive into (or past) the resting screen.
        AlertData emptyPriority;
        drawSecondaryAlertCards(nullptr, 0, emptyPriority, false);

        lastRestingPaletteRevision_ = paletteRevision_;
        lastRestingProfileSlot_ = profileSlot;

        // Log screen mode transition for debugging display refresh issues
        if (currentScreen_ != ScreenMode::Resting) {
            DISPLAY_LOG("[DISP] Screen mode: %d -> Resting (showResting)\n", (int)currentScreen_);
            perfRecordDisplayScreenTransition(perfScreenForMode(currentScreen_), PerfDisplayScreen::Resting, millis());
        }
        currentScreen_ = ScreenMode::Resting;

        DISPLAY_FLUSH();
    } else if (profileChanged) {
        perfRecordDisplayRenderPath(restoreRender ? PerfDisplayRenderPath::Restore
                                                  : PerfDisplayRenderPath::RestingIncremental);
        renderStartUs = micros();
        recordRenderTiming = true;
        // Only the profile changed while already resting; redraw just the indicator
        drawProfileIndicator(profileSlot);
        lastRestingProfileSlot_ = profileSlot;
        // Flush exactly what the leaves annotated in DrawnRegion — the profile
        // indicator rect at [499,629)×[152,172) plus the battery corner/percent
        // restore — rather than hand-picked rects. The previous fixed rects
        // (top strip + left column) predated the indicator's move to the
        // bottom-right and did not contain the painted pixels, leaving the
        // panel stale until the next full flush (Valentine's Law: bounded
        // drift — the panel must not stay stale across frames).
        if (!drawnRegion_.empty()) {
            flushRegion(drawnRegion_.x(), drawnRegion_.y(), drawnRegion_.w(), drawnRegion_.h());
            drawnRegion_.reset();
        }
    }

    // Reset lastState_ so next update() detects changes from this "resting" state
    lastState_ = DisplayState(); // All defaults: bands=0, arrows=0, bars=0, hasMode=false, modeChar=0

    if (recordRenderTiming) {
        perfRecordDisplayScenarioRenderUs(micros() - renderStartUs);
    }
}

// ============================================================================
// forceNextRedraw / resetChangeTracking
// ============================================================================

void V1Display::forceNextRedraw() {
    perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::ForceRedraw);
    // Reset lastState_ to force next update() to detect all changes and redraw
    lastState_ = DisplayState();
    // Set screen mode to Unknown so any next update/showResting detects a screen change
    currentScreen_ = ScreenMode::Unknown;
    // Reset the singleton-scoped render tracking variables (volume, mode,
    // arrows, etc.) so the single production display path fully redraws.
    resetChangeTracking();
}

void V1Display::resetChangeTracking() {
    dirty_.resetTracking = true;
}

// ============================================================================
// showScanning
// ============================================================================

void V1Display::showScanning() {
    const PerfDisplayRenderScenario renderScenario = perfGetDisplayRenderScenario();
    const bool restoreRender = (renderScenario == PerfDisplayRenderScenario::Restore);
    const unsigned long renderStartUs = micros();
    if (restoreRender) {
        perfRecordDisplayRenderPath(PerfDisplayRenderPath::Restore);
    }
    // Always use multi-alert layout positioning
    dirty_.multiAlert = true;

    const V1Settings& s = settingsManager.get();

    // Clear and draw the base frame
    drawBaseFrame();

    // Draw idle state elements
    drawTopCounter('0', false, true);
    // Volume indicator not shown in scanning state (no DisplayState available)
    drawBandIndicators(0, false);
    drawVerticalSignalBars(0, 0, BAND_KA, false);
    drawDirectionArrow(DIR_NONE, false);
    drawMuteIcon(false);
    syncTopIndicators(millis());
    drawObdIndicator();
    drawAlpIndicator();
    drawProfileIndicator(currentProfileSlot_);

    // Draw "SCAN" with the same geometry as the live frequency text.
    const char* text = "SCAN";
    if (fontMgr_.segment7Ready) {
        // Use Segment7 TTF font at the frequency font size and baseline.
        const int fontSize = DisplayLayout::FREQUENCY_OFR_FONT_SIZE;
        const int leftMargin = DisplayLayout::FREQUENCY_OFR_LEFT_MARGIN;
        const int y = DisplayLayout::frequencyOfrY();
        const int maxWidth = DisplayLayout::frequencyOfrMaxWidth();

        FT_BBox bbox = fontMgr_.segment7.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, text);
        const int glyphXMin = static_cast<int>(bbox.xMin);
        const int glyphXMax = static_cast<int>(bbox.xMax);
        const int textWidth = glyphXMax - glyphXMin;
        int x = leftMargin + (maxWidth - textWidth) / 2;
        if (x < leftMargin)
            x = leftMargin;

        constexpr int kClearPadPx = 12;
        int clearLeft = x + glyphXMin - kClearPadPx;
        if (clearLeft < DisplayLayout::kFrequencyZoneRect.x) {
            clearLeft = DisplayLayout::kFrequencyZoneRect.x;
        }
        int clearRight = x + glyphXMax + kClearPadPx;
        const int clearMaxX = DisplayLayout::kFrequencyZoneRect.x + DisplayLayout::kFrequencyZoneRect.w;
        if (clearRight > clearMaxX)
            clearRight = clearMaxX;
        const int clearY = y - 8;
        int clearH = fontSize + 16;
        if (clearY + clearH > DisplayLayout::CONTENT_BOTTOM_Y) {
            clearH = DisplayLayout::CONTENT_BOTTOM_Y - clearY;
        }
        if (clearRight > clearLeft && clearH > 0) {
            FILL_RECT(clearLeft, clearY, clearRight - clearLeft, clearH, PALETTE_BG);
        }

        const Rgb888 bg = rgb565ToRgb888(PALETTE_BG);
        const Rgb888 fg = rgb565ToRgb888(s.colorBandKa);
        fontMgr_.segment7.setBackgroundColor(bg.r, bg.g, bg.b);
        fontMgr_.segment7.setFontSize(fontSize);
        fontMgr_.segment7.setFontColor(fg.r, fg.g, fg.b);
        fontMgr_.segment7.setCursor(x, y);
        fontMgr_.segment7.printf("%s", text);
    } else {
        // Fallback: software 14-segment display using the frequency fallback scale.
        const float scale = DisplayLayout::FREQUENCY_FALLBACK_SCALE;
        SegMetrics m = segMetrics(scale);
        const int y = DisplayLayout::frequencyFallbackY(m.digitH);
        const int width = measureSevenSegmentText(text, scale); // Same geometry for 14-seg

        const int leftMargin = DisplayLayout::FREQUENCY_FALLBACK_LEFT_MARGIN;
        const int maxWidth = DisplayLayout::frequencyFallbackMaxWidth();
        int x = leftMargin + (maxWidth - width) / 2;
        if (x < leftMargin)
            x = leftMargin;

        FILL_RECT(x - 4, y - 4, width + 8, m.digitH + 8, PALETTE_BG);
        draw14SegmentText(text, x, y, scale, s.colorBandKa, PALETTE_BG);
    }

    // Reset lastState_
    lastState_ = DisplayState();

    DISPLAY_FLUSH();

    if (currentScreen_ != ScreenMode::Scanning) {
        perfRecordDisplayScreenTransition(perfScreenForMode(currentScreen_), PerfDisplayScreen::Scanning, millis());
    }
    currentScreen_ = ScreenMode::Scanning;
    lastRestingProfileSlot_ = -1;
    perfRecordDisplayScenarioRenderUs(micros() - renderStartUs);
}

// ============================================================================
// showBootSplash
// ============================================================================

void V1Display::showBootSplash() {
    const unsigned long splashStartMs = millis();
    drawBaseFrame();

    // Decode the RLE-compressed V1 Simple logo.  Each row must be decoded
    // sequentially (RLE state is linear), but we stage the full 640×172×2
    // byte image (~215 KiB) in PSRAM and hand it to Arduino_GFX as one blit
    // instead of 172 per-row blits.  Arduino_GFX's full-bitmap path takes a
    // single bounds-check + tight pixel loop; the per-row path re-enters
    // that machinery once per row, with a non-trivial fixed cost each time.
    // Boot-path only — the PSRAM buffer is freed before backlight enable.
    const unsigned long logoStartMs = millis();
    constexpr size_t kLogoPixelCount =
        static_cast<size_t>(V1SIMPLE_LOGO_WIDTH) * static_cast<size_t>(V1SIMPLE_LOGO_HEIGHT);
    constexpr size_t kLogoBytes = kLogoPixelCount * sizeof(uint16_t);
    uint16_t* logoBuffer = static_cast<uint16_t*>(heap_caps_malloc(kLogoBytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    if (logoBuffer) {
        // Decode all rows into the contiguous buffer, then one blit.
        for (int sy = 0; sy < V1SIMPLE_LOGO_HEIGHT; sy++) {
            decodeV1SimpleLogoRow(static_cast<uint16_t>(sy),
                                  logoBuffer + static_cast<size_t>(sy) * V1SIMPLE_LOGO_WIDTH);
        }
        TFT_CALL(draw16bitRGBBitmap)(0, 0, logoBuffer, V1SIMPLE_LOGO_WIDTH, V1SIMPLE_LOGO_HEIGHT);
        heap_caps_free(logoBuffer);
    } else {
        // PSRAM allocation failed (shouldn't happen on this board, but keep
        // the splash visible rather than crashing the boot screen).  Fall
        // back to the row-by-row path with a stack row buffer.
        uint16_t rowBuffer[V1SIMPLE_LOGO_WIDTH];
        for (int sy = 0; sy < V1SIMPLE_LOGO_HEIGHT; sy++) {
            decodeV1SimpleLogoRow(static_cast<uint16_t>(sy), rowBuffer);
            TFT_CALL(draw16bitRGBBitmap)(0, sy, rowBuffer, V1SIMPLE_LOGO_WIDTH, 1);
        }
    }
    const unsigned long logoMs = millis() - logoStartMs;

    // Draw version number in bottom-right corner
    GFX_setTextDatum(BR_DATUM); // Bottom-right alignment
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(0x7BEF, PALETTE_BG); // Gray text (mid-gray RGB565)
    GFX_drawString(tft_, "v" FIRMWARE_VERSION, SCREEN_WIDTH - 8, SCREEN_HEIGHT - 6);

    // Flush canvas to display before enabling backlight
    const unsigned long flushStartMs = millis();
    DISPLAY_FLUSH();
    const unsigned long flushMs = millis() - flushStartMs;

    // Turn on backlight now that splash is drawn
    // Waveshare 3.49" has INVERTED backlight: 0=full on, 255=off
    analogWrite(LCD_BL, 0); // Full brightness (inverted)
    Serial.println("Backlight ON (post-splash, inverted)");
    Serial.printf("[BootTiming] splash total=%lu logo=%lu flush=%lu\n", millis() - splashStartMs, logoMs, flushMs);
}

// ============================================================================
// showShutdown
// ============================================================================

void V1Display::showShutdown() {
    // Clear screen
    TFT_CALL(fillScreen)(PALETTE_BG);

    // Draw "GOODBYE" message centered
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(3);
    TFT_CALL(setTextColor)(PALETTE_TEXT, PALETTE_BG);
    GFX_drawString(tft_, "GOODBYE", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - 20);

    // Draw smaller "Powering off..." below
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(PALETTE_GRAY, PALETTE_BG);
    GFX_drawString(tft_, "Powering off...", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 + 20);

    // Flush to display
    DISPLAY_FLUSH();
}

// ============================================================================
// showLowBattery
// ============================================================================

void V1Display::showLowBattery() {
    // Clear screen
    TFT_CALL(fillScreen)(PALETTE_BG);

    // Draw large battery outline in center
    const int battW = 120;
    const int battH = 60;
    const int battX = (SCREEN_WIDTH - battW) / 2;
    const int battY = (SCREEN_HEIGHT - battH) / 2 - 20;
    const int capW = 12;
    const int capH = 24;

    // Draw battery outline in red
    uint16_t redColor = 0xF800;
    DRAW_RECT(battX, battY, battW, battH, redColor);
    FILL_RECT(battX + battW, battY + (battH - capH) / 2, capW, capH, redColor);

    // Draw single bar (low)
    const int padding = 8;
    FILL_RECT(battX + padding, battY + padding, 20, battH - 2 * padding, redColor);

    // Draw "LOW BATTERY" text below
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(redColor, PALETTE_BG);
    GFX_drawString(tft_, "LOW BATTERY", SCREEN_WIDTH / 2, battY + battH + 30);

    // Flush to display
    DISPLAY_FLUSH();
}

// ============================================================================
// showStealth — blank screen with OBD speed centered
// ============================================================================

void V1Display::showStealth(float speedMph, bool speedValid) {
    dirty_.multiAlert = true;
    multiAlertMode_ = false;
    persistedMode_ = false;

    const bool displaySpeedValid = speedValid && speedMph >= 0.0f;
    const int roundedSpeedMph = displaySpeedValid ? static_cast<int>(speedMph + 0.5f) : -1;

    // External status setters may have drawn into the shared canvas before the
    // pipeline arrives here. Preserve today's safe behavior: any pending draw
    // still gets erased by a full stealth repaint instead of being hidden by a
    // cache-hit skip.
    const bool hadPendingExternalDraws = !drawnRegion_.empty();
    drawnRegion_.reset();
    arrowVisibilityForceFullFlush_ = false;

    if (currentScreen_ == ScreenMode::Stealth && !dirty_.resetTracking && !hadPendingExternalDraws &&
        lastStealthPaletteRevision_ == paletteRevision_ && lastStealthSpeedValid_ == displaySpeedValid &&
        lastStealthRoundedMph_ == roundedSpeedMph) {
        perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::CacheHitSkipFlush);
        return;
    }

    // Clear to background
    TFT_CALL(fillScreen)(PALETTE_BG);

    // Build speed string
    char speedText[16];
    if (displaySpeedValid) {
        snprintf(speedText, sizeof(speedText), "%d MPH", roundedSpeedMph);
    } else {
        snprintf(speedText, sizeof(speedText), "-- MPH");
    }

    // Draw centered in white — large, clean, readable
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(10);
    TFT_CALL(setTextColor)(0xFFFF, PALETTE_BG);
    GFX_drawString(tft_, speedText, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);

    DISPLAY_FLUSH();

    dirty_.resetTracking = false;
    currentScreen_ = ScreenMode::Stealth;
    lastStealthPaletteRevision_ = paletteRevision_;
    lastStealthSpeedValid_ = displaySpeedValid;
    lastStealthRoundedMph_ = roundedSpeedMph;
}
