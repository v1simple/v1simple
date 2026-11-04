#include "display.h"
#include <cstring>
#include "display_draw.h"
#include "display_dirty_flags.h"
#include "display_element_caches.h"
#include "display_palette.h"
#include "display_text.h"
#include "modules/alp/alp_runtime_module.h"
#include "modules/alp/alp_laser_event.h"

void V1Display::drawBaseFrame(const ResolvedPaintStatus& paint) {
    TFT_CALL(fillScreen)(PALETTE_BG);
    prepareFullRedrawNoClear(paint);
}

void V1Display::prepareFullRedrawNoClear(const ResolvedPaintStatus& paint) {
    bleProxyDrawn_ = false;         // Force indicator redraw after full clears
    dirty_.setIndicatorFlags();     // Sets obdIndicator + alpIndicator only; other fields untouched
    elementCaches_.invalidateAll(); // Directly zeros all per-element render caches
    drawBLEProxyIndicator(paint);        // Redraw BLE icon after screen clear
}

void V1Display::drawObdIndicator(const ResolvedPaintStatus& paint) {
#if defined(DISPLAY_WAVESHARE_349)
    const bool wantShow = paint.obdShown;
    const bool curConnected = paint.obdConnected;
    const bool curAttention = paint.obdAttention;

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

    const uint16_t textColor = paint.obdColor;

    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(textColor, PALETTE_BG);
    GFX_drawString(tft_, "OBD", badgeRect.x + badgeRect.w / 2, badgeRect.y + badgeRect.h / 2);
#endif
}

void V1Display::drawGpsIndicator(const ResolvedPaintStatus& paint) {
#if defined(DISPLAY_WAVESHARE_349)
    const bool wantShow = paint.gpsShown;
    const uint8_t sats = paint.gpsSatellites;

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

void V1Display::drawAlpIndicator(const ResolvedPaintStatus& paint) {
#if defined(DISPLAY_WAVESHARE_349)
    const bool wantShow = paint.alpShown;

    if (!dirty_.alpIndicator && elementCaches_.alp.valid && wantShow == elementCaches_.alp.lastShown &&
        paint.alpState == elementCaches_.alp.lastState && paint.alpHeartbeat == elementCaches_.alp.lastHbByte1 &&
        paint.alpAlertActive == elementCaches_.alp.lastAlpEventActive) {
        return;
    }
    dirty_.alpIndicator = false;
    elementCaches_.alp.valid = true;
    elementCaches_.alp.lastShown = wantShow;
    elementCaches_.alp.lastState = paint.alpState;
    elementCaches_.alp.lastHbByte1 = paint.alpHeartbeat;
    elementCaches_.alp.lastAlpEventActive = paint.alpAlertActive;

    const DisplayLayout::DisplayRect badgeRect = DisplayLayout::alpBadgeRect();

    drawnRegion_.add(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h);
    FILL_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, PALETTE_BG);
    if (!wantShow) {
        return;
    }

    const uint16_t textColor = paint.alpColor;

    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(textColor, PALETTE_BG);
    GFX_drawString(tft_, "ALP", badgeRect.x + badgeRect.w / 2, badgeRect.y + badgeRect.h / 2);
#endif
}

void V1Display::drawStatusText(const char* text, uint16_t color) {
    TFT_CALL(setTextColor)(color, PALETTE_BG);
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    GFX_drawString(tft_, text, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
}

void V1Display::refreshObdIndicator(const DisplayPaintContext& context) {
    const ResolvedPaintStatus paint = resolvePaintStatus(context, nullptr);
    drawObdIndicator(paint);
}
