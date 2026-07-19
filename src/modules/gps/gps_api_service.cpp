#include "gps_api_service.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "modules/gps/gps_runtime_module.h"
#include "modules/gps/gps_runtime_status.h"
#include "modules/wifi/wifi_api_response.h"
#include "settings.h"
#include "settings_sanitize.h"

namespace GpsApiService {

namespace {

void sendMaintenanceModeError(WebServer& server) {
    JsonDocument doc;
    doc["error"] = "maintenance_mode";
    doc["message"] = "GPS runtime status is not available in maintenance mode";
    WifiApiResponse::sendJsonDocument(server, 409, doc);
}

void sendRuntimeUnavailableError(WebServer& server) {
    server.send(503, "application/json", "{\"error\":\"gps runtime not wired\"}");
}

bool requiresLiveRuntime(const Runtime& runtime) {
    return !runtime.maintenanceBootActive;
}

} // namespace

void handleApiConfigGet(WebServer& server, SettingsManager& settings, const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);
    const V1Settings& s = settings.get();
    JsonDocument doc;
    doc["gpsEnabled"] = s.gpsEnabled;
    doc["gpsBaud"] = s.gpsBaud;
    doc["gpsEnablePinActiveHigh"] = s.gpsEnablePinActiveHigh;
    doc["gpsLogUtcToPerf"] = s.gpsLogUtcToPerf;
    doc["gpsLogUtcToAlp"] = s.gpsLogUtcToAlp;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiConfigSave(WebServer& server, SettingsManager& settings, GpsRuntimeModule* gpsRuntimePtr,
                         const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);

    // Maintenance saves are intentionally persistence-only and must not
    // require the live UART runtime that maintenance boot skips.
    if (requiresLiveRuntime(runtime) && !gpsRuntimePtr) {
        sendRuntimeUnavailableError(server);
        return;
    }

    JsonDocument body;
    if (server.hasArg("plain")) {
        const String requestBody = server.arg("plain");
        const DeserializationError err = deserializeJson(body, requestBody.c_str());
        if (err) {
            JsonDocument errDoc;
            WifiApiResponse::setErrorAndMessage(errDoc, "Invalid JSON");
            WifiApiResponse::sendJsonDocument(server, 400, errDoc);
            return;
        }
    }

    DeviceSettingsUpdate update{};

    if (body["gpsEnabled"].is<bool>()) {
        update.hasGpsEnabled = true;
        update.gpsEnabled = body["gpsEnabled"].as<bool>();
    }
    if (body["gpsBaud"].is<uint32_t>()) {
        update.hasGpsBaud = true;
        update.gpsBaud = sanitizeGpsBaudValue(body["gpsBaud"].as<uint32_t>());
    }
    // Deprecated/no-op compatibility field. Supported GPS wiring does not drive
    // EN, so old clients may send this key but it must not mutate live state.
    if (body["gpsLogUtcToPerf"].is<bool>()) {
        update.hasGpsLogUtcToPerf = true;
        update.gpsLogUtcToPerf = body["gpsLogUtcToPerf"].as<bool>();
    }
    if (body["gpsLogUtcToAlp"].is<bool>()) {
        update.hasGpsLogUtcToAlp = true;
        update.gpsLogUtcToAlp = body["gpsLogUtcToAlp"].as<bool>();
    }

    settings.applyDeviceSettingsUpdate(update, runtime.maintenanceBootActive
                                                   ? SettingsPersistMode::Immediate
                                                   : SettingsPersistMode::ImmediateNvsDeferredBackup);

    // Maintenance boot intentionally skips GPS runtime init, so saving still
    // persists the new values to NVS (above) but we must not bring the UART
    // up here. The applied settings take effect on the next normal boot.
    if (runtime.maintenanceBootActive) {
        JsonDocument ok;
        ok["success"] = true;
        ok["message"] = "GPS settings saved; live runtime resumes on next normal boot.";
        WifiApiResponse::sendJsonDocument(server, 200, ok);
        return;
    }

    // Apply changes to the live runtime — no reboot required.
    GpsRuntimeModule& gpsRuntime = *gpsRuntimePtr;
    const V1Settings& s = settings.get();
    if (update.hasGpsBaud) {
        gpsRuntime.setBaud(s.gpsBaud);
    }
    if (update.hasGpsEnablePinActiveHigh) {
        gpsRuntime.setEnablePinActiveHigh(s.gpsEnablePinActiveHigh);
    }
    if (update.hasGpsEnabled) {
        gpsRuntime.setEnabled(s.gpsEnabled);
    } else if (update.hasGpsBaud || update.hasGpsEnablePinActiveHigh) {
        // Baud changed: restart UART with new parameters. The polarity flag is
        // deprecated/no-op but retained here for non-HTTP internal callers.
        if (gpsRuntime.isEnabled()) {
            gpsRuntime.setEnabled(false);
            gpsRuntime.setEnabled(true);
        }
    }

    JsonDocument ok;
    ok["success"] = true;
    WifiApiResponse::sendJsonDocument(server, 200, ok);
}

void handleApiStatus(WebServer& server, GpsRuntimeModule* gpsRuntime, const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);
    if (runtime.maintenanceBootActive) {
        sendMaintenanceModeError(server);
        return;
    }
    if (!gpsRuntime) {
        sendRuntimeUnavailableError(server);
        return;
    }

    const uint32_t nowMs = millis();
    const GpsRuntimeStatus s = gpsRuntime->snapshot(nowMs);

    JsonDocument doc;

    doc["enabled"] = s.enabled;
    doc["moduleDetected"] = s.moduleDetected;
    doc["detectionTimedOut"] = s.detectionTimedOut;
    doc["parserActive"] = s.parserActive;

    doc["hasFix"] = s.hasFix;
    doc["stableHasFix"] = s.stableHasFix;
    doc["satellites"] = s.satellites;
    doc["stableSatellites"] = s.stableSatellites;
    doc["hdop"] = (isnan(s.hdop) ? -1.0f : s.hdop);
    doc["speedMph"] = s.speedMph;
    doc["locationValid"] = s.locationValid;

    doc["fixAgeMs"] = (s.fixAgeMs == UINT32_MAX ? -1 : static_cast<int32_t>(s.fixAgeMs));
    doc["stableFixAgeMs"] = (s.stableFixAgeMs == UINT32_MAX ? -1 : static_cast<int32_t>(s.stableFixAgeMs));
    doc["sampleAgeMs"] = (s.sampleAgeMs == UINT32_MAX ? -1 : static_cast<int32_t>(s.sampleAgeMs));
    doc["lastSentenceAgeMs"] = (s.lastSentenceAgeMs == UINT32_MAX ? -1 : static_cast<int32_t>(s.lastSentenceAgeMs));
    doc["firstFixMs"] = s.firstFixMs;

    JsonObject counters = doc["counters"].to<JsonObject>();
    counters["sentencesParsed"] = s.sentencesParsed;
    counters["parseFailures"] = s.parseFailures;
    counters["checksumFailures"] = s.checksumFailures;
    counters["sentencesUnknown"] = s.sentencesUnknown;
    counters["bufferOverruns"] = s.bufferOverruns;
    counters["bytesRead"] = s.bytesRead;
    counters["enableTransitions"] = s.enableTransitions;

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

} // namespace GpsApiService
