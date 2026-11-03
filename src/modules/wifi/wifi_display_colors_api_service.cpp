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
    bool touches = anySignalBarColorSet(update);
    DisplaySettingsContract::forEachScalar([&](const auto& field) {
        touches |= field.renderedVisual && update.*(field.present);
    });
    return touches;
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

    DisplaySettingsContract::forEachColor([&](const auto& field) {
        const char* key = server.hasArg(field.httpKey) ? field.httpKey : field.legacyHttpKey;
        if (key && server.hasArg(key)) {
            update.*(field.present) = true;
            update.*(field.incoming) = static_cast<uint16_t>(server.arg(key).toInt());
        }
    });
    if (update.hasColorBogey || update.hasColorFrequency || update.hasColorArrowFront || update.hasColorArrowSide ||
        update.hasColorArrowRear || update.hasColorBandL || update.hasColorBandKa || update.hasColorBandK ||
        update.hasColorBandX) {
        Serial.printf("[HTTP] Saving colors: bogey=%d freq=%d arrowF=%d arrowS=%d arrowR=%d\n",
                      update.hasColorBogey ? update.colorBogey : s.colorBogey,
                      update.hasColorFrequency ? update.colorFrequency : s.colorFrequency,
                      update.hasColorArrowFront ? update.colorArrowFront : s.colorArrowFront,
                      update.hasColorArrowSide ? update.colorArrowSide : s.colorArrowSide,
                      update.hasColorArrowRear ? update.colorArrowRear : s.colorArrowRear);
    }
    // v11 clients may still send barS1..barS8. Collapse that shape onto the
    // physical six cells, using current colours for omitted arguments.
    {
        uint16_t compatibilitySegments[8];
        DisplayVisualContract::expandSixBarColorsToEight(s.colorBars, compatibilitySegments);
        bool haveCompatibilitySegments = false;
        for (int barIndex = 0; barIndex < 8; ++barIndex) {
            const char* arg = DisplaySettingsContract::kLegacyBars[barIndex].httpKey;
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
        const char* arg = DisplaySettingsContract::kBars[barIndex].httpKey;
        if (server.hasArg(arg)) {
            update.hasColorBar[barIndex] = true;
            update.colorBars[barIndex] = server.arg(arg).toInt();
        }
    }
    for (const auto& field : DisplaySettingsContract::kFlags) {
        if (server.hasArg(field.httpKey)) {
            update.*(field.present) = true;
            update.*(field.incoming) = server.arg(field.httpKey) == "true" || server.arg(field.httpKey) == "1";
        }
    }
    const auto& brightness = DisplaySettingsContract::kBrightness;
    if (server.hasArg(brightness.httpKey)) {
        nextBrightness = static_cast<uint8_t>(std::clamp<int>(server.arg(brightness.httpKey).toInt(),
            DisplaySettingsContract::kBrightnessMin, DisplaySettingsContract::kBrightnessMax));
        update.*(brightness.present) = true;
        update.*(brightness.incoming) = nextBrightness;
        hasBrightness = true;
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

    // Schedule through the preview module: showDisplayDemo blocks handleClient
    // with three SPI flushes (~120 ms). Zero requests the full sequence;
    // save/reset previews supply a finite hold.
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
    // Descriptor names have static lifetime, preserving literal-key storage.
    const auto writeColor = [&](const auto& field) {
        if (field.legacyHttpKey) doc[JsonString(field.legacyHttpKey, true)] = s.*(field.value);
        doc[JsonString(field.httpKey, true)] = s.*(field.value);
    };
    for (const auto& field : DisplaySettingsContract::kColorsBeforeBars) writeColor(field);
    // Six physical-segment values are authoritative.
    for (int barIndex = 0; barIndex < 6; ++barIndex) {
        doc[JsonString(DisplaySettingsContract::kBars[barIndex].httpKey, true)] = s.colorBars[barIndex];
    }
    // Retain an expanded v11 response shape for older clients.
    uint16_t compatibilitySegments[8];
    DisplayVisualContract::expandSixBarColorsToEight(s.colorBars, compatibilitySegments);
    for (int barIndex = 0; barIndex < 8; ++barIndex) {
        doc[JsonString(DisplaySettingsContract::kLegacyBars[barIndex].httpKey, true)] = compatibilitySegments[barIndex];
    }
    for (const auto& field : DisplaySettingsContract::kColorsAfterBars) writeColor(field);
    for (const auto& field : DisplaySettingsContract::kFlags) doc[JsonString(field.httpKey, true)] = s.*(field.value);
    const auto& brightness = DisplaySettingsContract::kBrightness;
    doc[JsonString(brightness.httpKey, true)] = s.*(brightness.value);

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

} // namespace WifiDisplayColorsApiService
