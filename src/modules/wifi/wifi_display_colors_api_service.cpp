#include <cstdio>

#include "display_visual_contract.h"
#include "wifi_display_colors_api_service.h"

#include <algorithm>

#include <ArduinoJson.h>

#include "wifi_api_response.h"
#include "wifi_json_document.h"

namespace WifiDisplayColorsApiService {

namespace {

bool anySignalBarColorSet(const DisplaySettingsUpdate& update) {
    for (int barIndex = 0; barIndex < SIGNAL_BAR_COLOR_COUNT; ++barIndex) {
        if (update.hasColorBar[barIndex]) {
            return true;
        }
    }
    return false;
}

bool updateTouchesRenderedVisuals(const DisplaySettingsUpdate& update) {
    return update.hasColorBogey || update.hasColorFrequency || update.hasColorArrowFront || update.hasColorArrowSide ||
           update.hasColorArrowRear || update.hasColorBandL || update.hasColorBandKa || update.hasColorBandK ||
           update.hasColorBandX || update.hasColorBandPhoto || update.hasColorWiFiConnected ||
           update.hasColorBleConnected || update.hasColorBleDisconnected ||
           anySignalBarColorSet(update) || update.hasColorMuted || update.hasColorPersisted ||
           update.hasColorVolumeMain || update.hasColorVolumeMute || update.hasColorRssiV1 ||
           update.hasColorRssiProxy || update.hasColorObd || update.hasColorAlpConnected || update.hasColorAlpDli ||
           update.hasColorAlpLidActive || update.hasColorAlpAlert || update.hasFreqUseBandColor ||
           update.hasHideWifiIcon || update.hasHideProfileIndicator || update.hasHideBatteryIcon ||
           update.hasShowBatteryPercent || update.hasHideBleIcon || update.hasHideVolumeIndicator ||
           update.hasHideRssiIndicator;
}

} // namespace

void handleApiSave(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;

    if (!runtime.getSettings || !runtime.applySettingsUpdate) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    Serial.println("[HTTP] POST /api/display/settings");
#ifndef UNIT_TEST
    Serial.printf("[HTTP] Args count: %d\n", server.args());
#endif

    const V1Settings& s = runtime.getSettings(runtime.getSettingsCtx);
    DisplaySettingsUpdate update;
    bool hasBrightness = false;
    uint8_t nextBrightness = s.brightness;

    auto argBool = [&server](const char* key, bool fallback) -> bool {
        if (!server.hasArg(key))
            return fallback;
        return server.arg(key) == "true" || server.arg(key) == "1";
    };

    // Main display colors
    if (server.hasArg("bogey") || server.hasArg("freq") || server.hasArg("arrowFront") || server.hasArg("arrowSide") ||
        server.hasArg("arrowRear") || server.hasArg("bandL") || server.hasArg("bandKa") || server.hasArg("bandK") ||
        server.hasArg("bandX")) {
        if (server.hasArg("bogey")) {
            update.hasColorBogey = true;
            update.colorBogey = server.arg("bogey").toInt();
        }
        if (server.hasArg("freq")) {
            update.hasColorFrequency = true;
            update.colorFrequency = server.arg("freq").toInt();
        }
        if (server.hasArg("arrowFront")) {
            update.hasColorArrowFront = true;
            update.colorArrowFront = server.arg("arrowFront").toInt();
        }
        if (server.hasArg("arrowSide")) {
            update.hasColorArrowSide = true;
            update.colorArrowSide = server.arg("arrowSide").toInt();
        }
        if (server.hasArg("arrowRear")) {
            update.hasColorArrowRear = true;
            update.colorArrowRear = server.arg("arrowRear").toInt();
        }
        if (server.hasArg("bandL")) {
            update.hasColorBandL = true;
            update.colorBandL = server.arg("bandL").toInt();
        }
        if (server.hasArg("bandKa")) {
            update.hasColorBandKa = true;
            update.colorBandKa = server.arg("bandKa").toInt();
        }
        if (server.hasArg("bandK")) {
            update.hasColorBandK = true;
            update.colorBandK = server.arg("bandK").toInt();
        }
        if (server.hasArg("bandX")) {
            update.hasColorBandX = true;
            update.colorBandX = server.arg("bandX").toInt();
        }

        Serial.printf("[HTTP] Saving colors: bogey=%d freq=%d arrowF=%d arrowS=%d arrowR=%d\n",
                      update.hasColorBogey ? update.colorBogey : s.colorBogey,
                      update.hasColorFrequency ? update.colorFrequency : s.colorFrequency,
                      update.hasColorArrowFront ? update.colorArrowFront : s.colorArrowFront,
                      update.hasColorArrowSide ? update.colorArrowSide : s.colorArrowSide,
                      update.hasColorArrowRear ? update.colorArrowRear : s.colorArrowRear);
    }

    // Color groups
    if (server.hasArg("wifiConnected")) {
        update.hasColorWiFiConnected = true;
        update.colorWiFiConnected = server.arg("wifiConnected").toInt();
    } else if (server.hasArg("wifiIcon")) {
        // Compatibility adapter for existing clients: the retired idle-icon
        // key updates the one active WiFi indicator colour only when the
        // authoritative key is absent.
        update.hasColorWiFiConnected = true;
        update.colorWiFiConnected = server.arg("wifiIcon").toInt();
    }
    if (server.hasArg("bleConnected")) {
        update.hasColorBleConnected = true;
        update.colorBleConnected = server.arg("bleConnected").toInt();
    }
    if (server.hasArg("bleDisconnected")) {
        update.hasColorBleDisconnected = true;
        update.colorBleDisconnected = server.arg("bleDisconnected").toInt();
    }
    // v11 clients may still send barS1..barS8. Collapse that shape onto the
    // physical six cells, using current colours for omitted arguments.
    {
        uint16_t compatibilitySegments[8];
        DisplayVisualContract::expandSixBarColorsToEight(s.colorBars, compatibilitySegments);
        bool haveCompatibilitySegments = false;
        for (int barIndex = 0; barIndex < 8; ++barIndex) {
            char arg[8];
            std::snprintf(arg, sizeof(arg), "barS%d", barIndex + 1);
            if (server.hasArg(arg)) {
                compatibilitySegments[barIndex] = static_cast<uint16_t>(server.arg(arg).toInt());
                haveCompatibilitySegments = true;
            }
        }
        if (haveCompatibilitySegments) {
            uint16_t collapsed[6];
            DisplayVisualContract::collapseEightBarColorsToSix(compatibilitySegments, collapsed);
            for (int barIndex = 0; barIndex < 6; ++barIndex) {
                update.hasColorBar[barIndex] = true;
                update.colorBars[barIndex] = collapsed[barIndex];
            }
        }
    }
    // bar1..bar6 are authoritative and win if both request shapes are present.
    for (int barIndex = 0; barIndex < 6; ++barIndex) {
        char arg[8];
        std::snprintf(arg, sizeof(arg), "bar%d", barIndex + 1);
        if (server.hasArg(arg)) {
            update.hasColorBar[barIndex] = true;
            update.colorBars[barIndex] = server.arg(arg).toInt();
        }
    }
    if (server.hasArg("muted")) {
        update.hasColorMuted = true;
        update.colorMuted = server.arg("muted").toInt();
    }
    if (server.hasArg("bandPhoto")) {
        update.hasColorBandPhoto = true;
        update.colorBandPhoto = server.arg("bandPhoto").toInt();
    }
    if (server.hasArg("persisted")) {
        update.hasColorPersisted = true;
        update.colorPersisted = server.arg("persisted").toInt();
    }
    if (server.hasArg("volumeMain")) {
        update.hasColorVolumeMain = true;
        update.colorVolumeMain = server.arg("volumeMain").toInt();
    }
    if (server.hasArg("volumeMute")) {
        update.hasColorVolumeMute = true;
        update.colorVolumeMute = server.arg("volumeMute").toInt();
    }
    if (server.hasArg("rssiV1")) {
        update.hasColorRssiV1 = true;
        update.colorRssiV1 = server.arg("rssiV1").toInt();
    }
    if (server.hasArg("rssiProxy")) {
        update.hasColorRssiProxy = true;
        update.colorRssiProxy = server.arg("rssiProxy").toInt();
    }
    if (server.hasArg("obd")) {
        update.hasColorObd = true;
        update.colorObd = server.arg("obd").toInt();
    }
    if (server.hasArg("alpConnected")) {
        update.hasColorAlpConnected = true;
        update.colorAlpConnected = server.arg("alpConnected").toInt();
    }
    if (server.hasArg("alpDli")) {
        update.hasColorAlpDli = true;
        update.colorAlpDli = server.arg("alpDli").toInt();
    }
    if (server.hasArg("alpLidActive")) {
        update.hasColorAlpLidActive = true;
        update.colorAlpLidActive = server.arg("alpLidActive").toInt();
    }
    if (server.hasArg("alpAlert")) {
        update.hasColorAlpAlert = true;
        update.colorAlpAlert = server.arg("alpAlert").toInt();
    }

    // Display toggles
    if (server.hasArg("freqUseBandColor")) {
        update.hasFreqUseBandColor = true;
        update.freqUseBandColor = argBool("freqUseBandColor", s.freqUseBandColor);
    }
    if (server.hasArg("hideWifiIcon")) {
        update.hasHideWifiIcon = true;
        update.hideWifiIcon = argBool("hideWifiIcon", s.hideWifiIcon);
    }
    if (server.hasArg("hideProfileIndicator")) {
        update.hasHideProfileIndicator = true;
        update.hideProfileIndicator = argBool("hideProfileIndicator", s.hideProfileIndicator);
    }
    if (server.hasArg("hideBatteryIcon")) {
        update.hasHideBatteryIcon = true;
        update.hideBatteryIcon = argBool("hideBatteryIcon", s.hideBatteryIcon);
    }
    if (server.hasArg("showBatteryPercent")) {
        update.hasShowBatteryPercent = true;
        update.showBatteryPercent = argBool("showBatteryPercent", s.showBatteryPercent);
    }
    if (server.hasArg("hideBleIcon")) {
        update.hasHideBleIcon = true;
        update.hideBleIcon = argBool("hideBleIcon", s.hideBleIcon);
    }
    if (server.hasArg("hideVolumeIndicator")) {
        update.hasHideVolumeIndicator = true;
        update.hideVolumeIndicator = argBool("hideVolumeIndicator", s.hideVolumeIndicator);
    }
    if (server.hasArg("hideRssiIndicator")) {
        update.hasHideRssiIndicator = true;
        update.hideRssiIndicator = argBool("hideRssiIndicator", s.hideRssiIndicator);
    }

    // Misc sliders
    if (server.hasArg("brightness")) {
        int brightness = server.arg("brightness").toInt();
        brightness = std::max(1, std::min(brightness, 255));
        update.hasBrightness = true;
        update.brightness = static_cast<uint8_t>(brightness);
        hasBrightness = true;
        nextBrightness = static_cast<uint8_t>(brightness);
    }
    const bool visualRedrawNeeded = updateTouchesRenderedVisuals(update);
    const SettingsPersistResult result =
        runtime.applySettingsUpdate(update, runtime.applySettingsUpdateCtx);
    if (!result.success) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"settings_persist_failed\"}");
        return;
    }

    if (hasBrightness && runtime.setDisplayBrightness) {
        runtime.setDisplayBrightness(nextBrightness, runtime.setDisplayBrightnessCtx);
    }
    if (visualRedrawNeeded && runtime.forceDisplayRedraw) {
        runtime.forceDisplayRedraw(runtime.forceDisplayRedrawCtx);
    }

    // Trigger immediate display preview to show new colors (skip if requested)
    if (!server.hasArg("skipPreview") || (server.arg("skipPreview") != "true" && server.arg("skipPreview") != "1")) {
        if (runtime.requestColorPreviewHoldMs) {
            runtime.requestColorPreviewHoldMs(
                5500, runtime.requestColorPreviewHoldMsCtx); // Hold ~5.5s and cycle bands during preview.
        }
    }

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiReset(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;

    if (!runtime.resetDisplaySettings) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    const SettingsPersistResult result = runtime.resetDisplaySettings(runtime.resetDisplaySettingsCtx);
    if (!result.success) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"settings_persist_failed\"}");
        return;
    }
    if (runtime.forceDisplayRedraw) {
        runtime.forceDisplayRedraw(runtime.forceDisplayRedrawCtx);
    }

    // Trigger immediate display preview to show reset colors.
    if (runtime.requestColorPreviewHoldMs) {
        runtime.requestColorPreviewHoldMs(5500, runtime.requestColorPreviewHoldMsCtx);
    }

    server.send(200, "application/json", "{\"success\":true}");
}

static void handlePreviewImpl(WebServer& server, const Runtime& runtime) {
    const bool previewRunning =
        runtime.isColorPreviewRunning && runtime.isColorPreviewRunning(runtime.isColorPreviewRunningCtx);

    if (previewRunning) {
        if (runtime.cancelColorPreview) {
            runtime.cancelColorPreview(runtime.cancelColorPreviewCtx);
        }
        // main.cpp loop handles display restore based on V1 connection state
        server.send(200, "application/json", "{\"success\":true,\"active\":false}");
        return;
    }

    // showDisplayDemo() is unsuitable here because it performs three blocking SPI flushes
    // (~120ms) inside handleClient(), inflating wifiMaxUs.  The preview module
    // renders the first frame on the very next main-loop display phase.
    // Pass 0 as a signal to use the full diagnostic auto-duration. Short
    // save/reset previews pass an explicit non-zero hold duration instead.
    if (runtime.requestColorPreviewHoldMs) {
        runtime.requestColorPreviewHoldMs(0, runtime.requestColorPreviewHoldMsCtx);
    }
    server.send(200, "application/json", "{\"success\":true,\"active\":true}");
}

void handleApiPreview(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                      void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;
    handlePreviewImpl(server, runtime);
}

static void handleClearImpl(WebServer& server, const Runtime& runtime) {
    if (runtime.cancelColorPreview) {
        runtime.cancelColorPreview(runtime.cancelColorPreviewCtx);
    }
    // main.cpp loop handles display restore based on V1 connection state
    server.send(200, "application/json", "{\"success\":true,\"active\":false}");
}

void handleApiClear(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;
    handleClearImpl(server, runtime);
}

void handleApiGet(WebServer& server, const Runtime& runtime) {
    if (!runtime.getSettings) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    const V1Settings& s = runtime.getSettings(runtime.getSettingsCtx);

    WifiJson::Document doc;
    doc["bogey"] = s.colorBogey;
    doc["freq"] = s.colorFrequency;
    doc["arrowFront"] = s.colorArrowFront;
    doc["arrowSide"] = s.colorArrowSide;
    doc["arrowRear"] = s.colorArrowRear;
    doc["bandL"] = s.colorBandL;
    doc["bandKa"] = s.colorBandKa;
    doc["bandK"] = s.colorBandK;
    doc["bandX"] = s.colorBandX;
    doc["bandPhoto"] = s.colorBandPhoto;
    // Compatibility response alias for existing clients.
    doc["wifiIcon"] = s.colorWiFiConnected;
    doc["wifiConnected"] = s.colorWiFiConnected;
    doc["bleConnected"] = s.colorBleConnected;
    doc["bleDisconnected"] = s.colorBleDisconnected;
    // Six physical-segment values are authoritative.
    for (int barIndex = 0; barIndex < 6; ++barIndex) {
        char key[8];
        std::snprintf(key, sizeof(key), "bar%d", barIndex + 1);
        doc[key] = s.colorBars[barIndex];
    }
    // Retain an expanded v11 response shape for older clients.
    uint16_t compatibilitySegments[8];
    DisplayVisualContract::expandSixBarColorsToEight(s.colorBars, compatibilitySegments);
    for (int barIndex = 0; barIndex < 8; ++barIndex) {
        char key[8];
        std::snprintf(key, sizeof(key), "barS%d", barIndex + 1);
        doc[key] = compatibilitySegments[barIndex];
    }
    doc["muted"] = s.colorMuted;
    doc["persisted"] = s.colorPersisted;
    doc["volumeMain"] = s.colorVolumeMain;
    doc["volumeMute"] = s.colorVolumeMute;
    doc["rssiV1"] = s.colorRssiV1;
    doc["rssiProxy"] = s.colorRssiProxy;
    doc["obd"] = s.colorObd;
    doc["alpConnected"] = s.colorAlpConnected;
    doc["alpDli"] = s.colorAlpDli;
    doc["alpLidActive"] = s.colorAlpLidActive;
    doc["alpAlert"] = s.colorAlpAlert;
    doc["freqUseBandColor"] = s.freqUseBandColor;
    doc["hideWifiIcon"] = s.hideWifiIcon;
    doc["hideProfileIndicator"] = s.hideProfileIndicator;
    doc["hideBatteryIcon"] = s.hideBatteryIcon;
    doc["showBatteryPercent"] = s.showBatteryPercent;
    doc["hideBleIcon"] = s.hideBleIcon;
    doc["hideVolumeIndicator"] = s.hideVolumeIndicator;
    doc["hideRssiIndicator"] = s.hideRssiIndicator;
    doc["brightness"] = s.brightness;

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

} // namespace WifiDisplayColorsApiService
