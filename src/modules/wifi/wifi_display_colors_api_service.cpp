#include "wifi_display_colors_api_service.h"

#include <algorithm>

#include <ArduinoJson.h>

#include "wifi_api_response.h"
#include "wifi_json_document.h"

namespace WifiDisplayColorsApiService {

namespace {

bool updateTouchesRenderedVisuals(const DisplaySettingsUpdate& update) {
    return update.hasColorBogey || update.hasColorFrequency || update.hasColorArrowFront || update.hasColorArrowSide ||
           update.hasColorArrowRear || update.hasColorBandL || update.hasColorBandKa || update.hasColorBandK ||
           update.hasColorBandX || update.hasColorBandPhoto || update.hasColorWiFiIcon ||
           update.hasColorWiFiConnected || update.hasColorBleConnected || update.hasColorBleDisconnected ||
           update.hasColorBar1 || update.hasColorBar2 || update.hasColorBar3 || update.hasColorBar4 ||
           update.hasColorBar5 || update.hasColorBar6 || update.hasColorMuted || update.hasColorPersisted ||
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
    for (int i = 0; i < server.args(); i++) {
        Serial.printf("[HTTP] Arg %s = %s\n", server.argName(i).c_str(), server.arg(i).c_str());
    }
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
    if (server.hasArg("wifiIcon")) {
        update.hasColorWiFiIcon = true;
        update.colorWiFiIcon = server.arg("wifiIcon").toInt();
    }
    if (server.hasArg("wifiConnected")) {
        update.hasColorWiFiConnected = true;
        update.colorWiFiConnected = server.arg("wifiConnected").toInt();
    }
    if (server.hasArg("bleConnected")) {
        update.hasColorBleConnected = true;
        update.colorBleConnected = server.arg("bleConnected").toInt();
    }
    if (server.hasArg("bleDisconnected")) {
        update.hasColorBleDisconnected = true;
        update.colorBleDisconnected = server.arg("bleDisconnected").toInt();
    }
    if (server.hasArg("bar1")) {
        update.hasColorBar1 = true;
        update.colorBar1 = server.arg("bar1").toInt();
    }
    if (server.hasArg("bar2")) {
        update.hasColorBar2 = true;
        update.colorBar2 = server.arg("bar2").toInt();
    }
    if (server.hasArg("bar3")) {
        update.hasColorBar3 = true;
        update.colorBar3 = server.arg("bar3").toInt();
    }
    if (server.hasArg("bar4")) {
        update.hasColorBar4 = true;
        update.colorBar4 = server.arg("bar4").toInt();
    }
    if (server.hasArg("bar5")) {
        update.hasColorBar5 = true;
        update.colorBar5 = server.arg("bar5").toInt();
    }
    if (server.hasArg("bar6")) {
        update.hasColorBar6 = true;
        update.colorBar6 = server.arg("bar6").toInt();
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
    runtime.applySettingsUpdate(update, runtime.applySettingsUpdateCtx);

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

    runtime.resetDisplaySettings(runtime.resetDisplaySettingsCtx);
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

    // Do NOT call showDisplayDemo() here — it performs 3 blocking SPI flushes
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
    doc["wifiIcon"] = s.colorWiFiIcon;
    doc["wifiConnected"] = s.colorWiFiConnected;
    doc["bleConnected"] = s.colorBleConnected;
    doc["bleDisconnected"] = s.colorBleDisconnected;
    doc["bar1"] = s.colorBar1;
    doc["bar2"] = s.colorBar2;
    doc["bar3"] = s.colorBar3;
    doc["bar4"] = s.colorBar4;
    doc["bar5"] = s.colorBar5;
    doc["bar6"] = s.colorBar6;
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
