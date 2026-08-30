
#include "display.h"
#include "config.h"
#include "display_layout.h"
#include "display_draw.h"
#include "display_dirty_flags.h"
#include "display_palette.h"
#include "display_text.h"
#include "display_segments.h"
#include "display_flush.h"
#include "display_font_manager.h"
#include "v1simple_logo.h"
#include "settings.h"
#include <esp_heap_caps.h>

using namespace DisplaySegments;
using DisplayLayout::PRIMARY_ZONE_HEIGHT;


void V1Display::showDisconnected() {
    drawBaseFrame();
    drawStatusText("Disconnected", 0xF800); // Red
    drawWiFiIndicator();
    drawBatteryIndicator();
}


void V1Display::showMaintenanceMode(const char* ipAddress, bool stationMode) {
    dirty_.multiAlert = true;
    multiAlertMode_ = false;
    persistedMode_ = false;
    drawnRegion_.reset();
    arrowVisibilityForceFullFlush_ = false;

    drawBaseFrame();

    GFX_setTextDatum(MC_DATUM);

    TFT_CALL(setTextSize)(3);
    TFT_CALL(setTextColor)(0x07FF, PALETTE_BG); // Cyan
    GFX_drawString(tft_, "MAINTENANCE MODE", SCREEN_WIDTH / 2, 48);

    const bool hasIp = (ipAddress != nullptr && ipAddress[0] != '\0');

    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(PALETTE_TEXT, PALETTE_BG);
    const char* label = !hasIp ? "WiFi setup active" : (stationMode ? "Browse to:" : "Join WiFi, then browse to:");
    GFX_drawString(tft_, label, SCREEN_WIDTH / 2, 84);

    if (hasIp) {
        TFT_CALL(setTextSize)(3);
        TFT_CALL(setTextColor)(0x07E0, PALETTE_BG); // Green
        GFX_drawString(tft_, ipAddress, SCREEN_WIDTH / 2, 118);
    }

    // PALETTE_GRAY is too dark on black for the exit hint.
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(0x7BEF, PALETTE_BG); // Mid-grey
    GFX_drawString(tft_, "Hold BOOT 4s to exit", SCREEN_WIDTH / 2, 150);

    drawWiFiIndicator();
    drawBatteryIndicator();

    lastState_ = DisplayState();
    currentScreen_ = ScreenMode::Maintenance;
    lastRestingProfileSlot_ = -1;

    DISPLAY_FLUSH();
}


void V1Display::showResting(bool forceRedraw) {
    dirty_.multiAlert = true;
    multiAlertMode_ = false;

    // Preserve the connected-mode indicator; image2 is the blink off-phase.
    char savedBogeyChar = lastState_.bogeyCounterChar;
    bool savedBogeyDot = lastState_.bogeyCounterDot;

    bool paletteChanged = (lastRestingPaletteRevision_ != paletteRevision_);
    bool screenChanged = (currentScreen_ != ScreenMode::Resting);
    int profileSlot = currentProfileSlot_;
    bool profileChanged = (profileSlot != lastRestingProfileSlot_);

    if (forceRedraw || screenChanged || paletteChanged) {
        drawBaseFrame();

        char topChar = '0';
        bool topDot = true;
        if (bleCtx_.v1Connected && savedBogeyChar != 0) {
            topChar = savedBogeyChar;
            topDot = savedBogeyDot;
        }
        drawTopCounterPair(topChar, false, topDot);

        drawBandIndicators(0, false);

        drawVerticalSignalBars(0, 0, BAND_KA, false);

        drawDirectionArrow(DIR_NONE, false);

        drawFrequency(0, BAND_NONE);

        drawMuteIcon(false);
        syncTopIndicators(millis());
        drawObdIndicator();
        drawAlpIndicator();

        drawProfileIndicator(profileSlot);

        // Prevent stale live-alert cards from surviving the resting screen.
        AlertData emptyPriority;
        drawSecondaryAlertCards(nullptr, 0, emptyPriority, false);

        lastRestingPaletteRevision_ = paletteRevision_;
        lastRestingProfileSlot_ = profileSlot;

        currentScreen_ = ScreenMode::Resting;

        DISPLAY_FLUSH();
    } else if (profileChanged) {
        drawProfileIndicator(profileSlot);
        lastRestingProfileSlot_ = profileSlot;
        // Leaf-renderer regions include restored battery pixels that a static
        // profile rectangle can miss when geometry moves.
        if (!drawnRegion_.empty()) {
            flushRegion(drawnRegion_.x(), drawnRegion_.y(), drawnRegion_.w(), drawnRegion_.h());
            drawnRegion_.reset();
        }
    }

    lastState_ = DisplayState(); // All defaults: bands=0, arrows=0, bars=0, hasMode=false, modeChar=0

}


void V1Display::forceNextRedraw() {
    lastState_ = DisplayState();
    currentScreen_ = ScreenMode::Unknown;
    resetChangeTracking();
}

void V1Display::resetChangeTracking() {
    dirty_.resetTracking = true;
}


void V1Display::showScanning() {
    dirty_.multiAlert = true;

    const V1Settings& s = settings_.get();

    drawBaseFrame();

    drawTopCounter('0', false, true);
    drawBandIndicators(0, false);
    drawVerticalSignalBars(0, 0, BAND_KA, false);
    drawDirectionArrow(DIR_NONE, false);
    drawMuteIcon(false);
    syncTopIndicators(millis());
    drawObdIndicator();
    drawAlpIndicator();
    drawProfileIndicator(currentProfileSlot_);

    const char* text = "SCAN";
    if (fontMgr_.segment7Ready) {
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

    lastState_ = DisplayState();

    DISPLAY_FLUSH();

    currentScreen_ = ScreenMode::Scanning;
    lastRestingProfileSlot_ = -1;
}


void V1Display::showBootSplash() {
    const unsigned long splashStartMs = millis();
    drawBaseFrame();

    // RLE rows decode sequentially; one PSRAM-backed blit avoids 172 fixed-cost
    // draw calls. The buffer is freed before backlight enable.
    const unsigned long logoStartMs = millis();
    constexpr size_t kLogoPixelCount =
        static_cast<size_t>(V1SIMPLE_LOGO_WIDTH) * static_cast<size_t>(V1SIMPLE_LOGO_HEIGHT);
    constexpr size_t kLogoBytes = kLogoPixelCount * sizeof(uint16_t);
    uint16_t* logoBuffer = static_cast<uint16_t*>(heap_caps_malloc(kLogoBytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    if (logoBuffer) {
        for (int sy = 0; sy < V1SIMPLE_LOGO_HEIGHT; sy++) {
            decodeV1SimpleLogoRow(static_cast<uint16_t>(sy),
                                  logoBuffer + static_cast<size_t>(sy) * V1SIMPLE_LOGO_WIDTH);
        }
        TFT_CALL(draw16bitRGBBitmap)(0, 0, logoBuffer, V1SIMPLE_LOGO_WIDTH, V1SIMPLE_LOGO_HEIGHT);
        heap_caps_free(logoBuffer);
    } else {
        // Keep the splash functional if PSRAM allocation fails.
        uint16_t rowBuffer[V1SIMPLE_LOGO_WIDTH];
        for (int sy = 0; sy < V1SIMPLE_LOGO_HEIGHT; sy++) {
            decodeV1SimpleLogoRow(static_cast<uint16_t>(sy), rowBuffer);
            TFT_CALL(draw16bitRGBBitmap)(0, sy, rowBuffer, V1SIMPLE_LOGO_WIDTH, 1);
        }
    }
    const unsigned long logoMs = millis() - logoStartMs;

    GFX_setTextDatum(BR_DATUM); // Bottom-right alignment
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(0x7BEF, PALETTE_BG); // Gray text (mid-gray RGB565)
    GFX_drawString(tft_, "v" FIRMWARE_VERSION, SCREEN_WIDTH - 8, SCREEN_HEIGHT - 6);

    const unsigned long flushStartMs = millis();
    DISPLAY_FLUSH();
    const unsigned long flushMs = millis() - flushStartMs;

    setBrightness(255);
    Serial.println("Backlight ON (post-splash, inverted)");
    Serial.printf("[BootTiming] splash total=%lu logo=%lu flush=%lu\n", millis() - splashStartMs, logoMs, flushMs);
}


void V1Display::showShutdown() {
    TFT_CALL(fillScreen)(PALETTE_BG);

    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(3);
    TFT_CALL(setTextColor)(PALETTE_TEXT, PALETTE_BG);
    GFX_drawString(tft_, "GOODBYE", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - 20);

    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(PALETTE_GRAY, PALETTE_BG);
    GFX_drawString(tft_, "Powering off...", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 + 20);

    DISPLAY_FLUSH();
}


void V1Display::showLowBattery() {
    TFT_CALL(fillScreen)(PALETTE_BG);

    const int battW = 120;
    const int battH = 60;
    const int battX = (SCREEN_WIDTH - battW) / 2;
    const int battY = (SCREEN_HEIGHT - battH) / 2 - 20;
    const int capW = 12;
    const int capH = 24;

    uint16_t redColor = 0xF800;
    DRAW_RECT(battX, battY, battW, battH, redColor);
    FILL_RECT(battX + battW, battY + (battH - capH) / 2, capW, capH, redColor);

    const int padding = 8;
    FILL_RECT(battX + padding, battY + padding, 20, battH - 2 * padding, redColor);

    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(redColor, PALETTE_BG);
    GFX_drawString(tft_, "LOW BATTERY", SCREEN_WIDTH / 2, battY + battH + 30);

    DISPLAY_FLUSH();
}


void V1Display::showStealth(float speedMph, bool speedValid) {
    dirty_.multiAlert = true;
    multiAlertMode_ = false;
    persistedMode_ = false;

    const bool displaySpeedValid = speedValid && speedMph >= 0.0f;
    const int roundedSpeedMph = displaySpeedValid ? static_cast<int>(speedMph + 0.5f) : -1;

    // Pending external draws require a full stealth repaint before cache skipping.
    const bool hadPendingExternalDraws = !drawnRegion_.empty();
    drawnRegion_.reset();
    arrowVisibilityForceFullFlush_ = false;

    if (currentScreen_ == ScreenMode::Stealth && !dirty_.resetTracking && !hadPendingExternalDraws &&
        lastStealthPaletteRevision_ == paletteRevision_ && lastStealthSpeedValid_ == displaySpeedValid &&
        lastStealthRoundedMph_ == roundedSpeedMph) {
        return;
    }

    TFT_CALL(fillScreen)(PALETTE_BG);

    char speedText[16];
    if (displaySpeedValid) {
        snprintf(speedText, sizeof(speedText), "%d MPH", roundedSpeedMph);
    } else {
        snprintf(speedText, sizeof(speedText), "-- MPH");
    }

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
