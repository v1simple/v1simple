#include "gps_api_service.h"

#include <cstdio>

#include <Arduino.h>
#include <ArduinoJson.h>

#include "modules/gps/gps_runtime_module.h"
#include "modules/wifi/wifi_api_response.h"
#include "modules/wifi/wifi_json_document.h"
#include "settings.h"
#include "settings_sanitize.h"

namespace GpsApiService {

namespace {

void sendRuntimeUnavailableError(WebServer& server) {
    server.send(503, "application/json", "{\"error\":\"gps runtime not wired\"}");
}

void sendRequestError(WebServer& server, const char* message) {
    WifiJson::Document errDoc;
    WifiApiResponse::setErrorAndMessage(errDoc, message);
    WifiApiResponse::sendJsonDocument(server, 400, errDoc);
}

void sendFieldTypeError(WebServer& server, const char* key, const char* expected) {
    char message[96];
    std::snprintf(message, sizeof(message), "Field '%s' must be %s", key, expected);
    sendRequestError(server, message);
}

// Omitted fields preserve their current values, but a present field must match
// the documented wire type. JsonVariantConst::isUnbound() distinguishes an
// omitted key from an explicit JSON null, which is invalid input here.
bool readOptionalBool(WebServer& server, const JsonDocument& body, const char* key, bool& hasValue, bool& out) {
    JsonVariantConst value = body[key];
    if (value.isUnbound()) {
        return true;
    }
    if (!value.is<bool>()) {
        sendFieldTypeError(server, key, "a boolean");
        return false;
    }
    hasValue = true;
    out = value.as<bool>();
    return true;
}

bool readOptionalUint32(WebServer& server, const JsonDocument& body, const char* key, bool& hasValue, uint32_t& out) {
    JsonVariantConst value = body[key];
    if (value.isUnbound()) {
        return true;
    }
    if (!value.is<uint32_t>()) {
        sendFieldTypeError(server, key, "an unsigned integer");
        return false;
    }
    hasValue = true;
    out = value.as<uint32_t>();
    return true;
}

bool requiresLiveRuntime(const Runtime& runtime) {
    return !runtime.maintenanceBootActive;
}

} // namespace

void handleApiConfigGet(WebServer& server, SettingsManager& settings, const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);
    const V1Settings& s = settings.get();
    WifiJson::Document doc;
    doc["gpsEnabled"] = s.gpsEnabled;
    doc["gpsBaud"] = s.gpsBaud;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiConfigSave(WebServer& server, SettingsManager& settings, GpsRuntimeModule* gpsRuntimePtr,
                         const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);

    // Normal runtime availability is the route's first executable precondition,
    // matching the maintenance/runtime policy matrix and sibling OBD handler.
    // Maintenance saves are persistence-only and do not require the UART runtime.
    if (requiresLiveRuntime(runtime) && !gpsRuntimePtr) {
        sendRuntimeUnavailableError(server);
        return;
    }

    if (!server.hasArg("plain") || server.arg("plain").length() == 0) {
        sendRequestError(server, "Missing JSON body");
        return;
    }

    WifiJson::Document body;
    const String requestBody = server.arg("plain");
    const DeserializationError err = deserializeJson(body, requestBody.c_str());
    if (err) {
        sendRequestError(server, "Invalid JSON");
        return;
    }
    if (!body.is<JsonObjectConst>()) {
        sendRequestError(server, "JSON body must be an object");
        return;
    }

    DeviceSettingsUpdate update{};

    if (!readOptionalBool(server, body, "gpsEnabled", update.hasGpsEnabled, update.gpsEnabled)) {
        return;
    }
    if (!readOptionalUint32(server, body, "gpsBaud", update.hasGpsBaud, update.gpsBaud)) {
        return;
    }
    if (update.hasGpsBaud) {
        update.gpsBaud = sanitizeGpsBaudValue(update.gpsBaud);
    }
    const bool hasWritableSetting = update.hasGpsEnabled || update.hasGpsBaud;
    if (!hasWritableSetting) {
        sendRequestError(server, "No writable GPS settings provided");
        return;
    }

    const SettingsPersistResult persistResult =
        settings.applyDeviceSettingsUpdate(update, runtime.maintenanceBootActive
                                                        ? SettingsPersistMode::Immediate
                                                        : SettingsPersistMode::ImmediateNvsDeferredBackup);
    if (!persistResult.success) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"settings_persist_failed\"}");
        return;
    }

    // Maintenance boot intentionally skips GPS runtime init, so saving still
    // persists the new values to NVS (above) but we must not bring the UART
    // up here. The applied settings take effect on the next normal boot.
    if (runtime.maintenanceBootActive) {
        WifiJson::Document ok;
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
    if (update.hasGpsEnabled) {
        gpsRuntime.setEnabled(s.gpsEnabled);
    } else if (update.hasGpsBaud) {
        // Baud changed: restart UART with the new parameter.
        if (gpsRuntime.isEnabled()) {
            gpsRuntime.setEnabled(false);
            gpsRuntime.setEnabled(true);
        }
    }

    WifiJson::Document ok;
    ok["success"] = true;
    WifiApiResponse::sendJsonDocument(server, 200, ok);
}

} // namespace GpsApiService
