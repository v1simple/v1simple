#include "obd_api_service.h"

#include <algorithm>
#include <cstdio>

#include <ArduinoJson.h>

#include "modules/obd/obd_runtime_module.h"
#include "modules/wifi/wifi_api_response.h"
#include "modules/wifi/wifi_json_document.h"
#include "settings.h"

namespace ObdApiService {

namespace {

const char* commandKindName(ObdCommandKind kind) {
    switch (kind) {
    case ObdCommandKind::AT_INIT:
        return "at_init";
    case ObdCommandKind::SANITY:
        return "sanity";
    case ObdCommandKind::SPEED:
        return "speed";
    case ObdCommandKind::NONE:
    default:
        return "none";
    }
}

constexpr size_t MAX_OBD_DEVICE_NAME_LEN = 32;

bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

String normalizeObdDeviceAddress(const String& rawAddress) {
    String value = rawAddress;
    value.trim();
    value.replace("-", ":");
    value.toUpperCase();

    if (value.length() != 17) {
        return "";
    }

    for (int i = 0; i < 17; ++i) {
        const char c = value[i];
        if ((i + 1) % 3 == 0) {
            if (c != ':') {
                return "";
            }
            continue;
        }
        if (!isHex(c)) {
            return "";
        }
    }

    return value;
}

String sanitizeObdDeviceName(const String& raw) {
    String value = raw;
    value.trim();
    if (value.length() > MAX_OBD_DEVICE_NAME_LEN) {
        value = value.substring(0, MAX_OBD_DEVICE_NAME_LEN);
        value.trim();
    }
    return value;
}

void sendMaintenanceModeError(WebServer& server) {
    WifiJson::Document doc;
    doc["error"] = "maintenance_mode";
    doc["message"] = "OBD runtime endpoints are not available in maintenance mode";
    WifiApiResponse::sendJsonDocument(server, 409, doc);
}

void sendRuntimeUnavailableError(WebServer& server) {
    server.send(503, "application/json", "{\"error\":\"obd runtime not wired\"}");
}

void sendFieldTypeError(WebServer& server, const char* key, const char* expected) {
    char message[96];
    snprintf(message, sizeof(message), "Field '%s' must be %s", key, expected);
    WifiJson::Document errDoc;
    WifiApiResponse::setErrorAndMessage(errDoc, message);
    WifiApiResponse::sendJsonDocument(server, 400, errDoc);
}

// /api/obd/config takes partial updates, so an omitted key legitimately means
// "leave this setting alone". A key that is present but of the wrong type is a
// client bug, and answering it with a bare {"success":true} hides the fact that
// nothing was applied. Report it the way the rest of this service reports bad
// input: a 400 carrying error/message via WifiApiResponse::setErrorAndMessage,
// naming the offending field. Validation runs before any settings are applied, so
// a rejected request never lands a partial update.
bool readOptionalBool(WebServer& server, const JsonDocument& body, const char* key, bool& hasValue, bool& out) {
    JsonVariantConst value = body[key];
    if (value.isNull()) {
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

bool readOptionalInt(WebServer& server, const JsonDocument& body, const char* key, bool& hasValue, int& out) {
    JsonVariantConst value = body[key];
    if (value.isNull()) {
        return true;
    }
    if (!value.is<int>()) {
        sendFieldTypeError(server, key, "an integer");
        return false;
    }
    hasValue = true;
    out = value.as<int>();
    return true;
}

} // namespace

void handleApiConfigGet(WebServer& server, SettingsManager& settings, const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);
    const V1Settings& s = settings.get();
    WifiJson::Document doc;
    doc["enabled"] = s.obdEnabled;
    doc["minRssi"] = s.obdMinRssi;
    doc["obdScanWindowMs"] = s.obdScanWindowMs;
    doc["obdRetryIntervalMs"] = s.obdRetryIntervalMs;
    doc["proxyOpenWindowMs"] = s.proxyOpenWindowMs;
    doc["wifiOpenTimeoutMs"] = s.wifiOpenTimeoutMs;
    doc["v1SettleQuietMs"] = s.v1SettleQuietMs;
    doc["v1SettleFallbackMs"] = s.v1SettleFallbackMs;
    doc["cycleTeardownAckTimeoutMs"] = s.cycleTeardownAckTimeoutMs;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiStatus(WebServer& server, ObdRuntimeModule* obdRuntime, const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);
    if (runtime.maintenanceBootActive) {
        sendMaintenanceModeError(server);
        return;
    }
    if (!obdRuntime) {
        sendRuntimeUnavailableError(server);
        return;
    }
    ObdRuntimeStatus status = obdRuntime->snapshot(millis());
    WifiJson::Document doc;
    doc["enabled"] = status.enabled;
    doc["connected"] = status.connected;
    doc["securityReady"] = status.securityReady;
    doc["encrypted"] = status.encrypted;
    doc["bonded"] = status.bonded;
    doc["speedValid"] = status.speedValid;
    doc["speedMph"] = status.speedMph;
    doc["speedAgeMs"] = status.speedAgeMs;
    doc["rssi"] = status.rssi;
    doc["scanInProgress"] = status.scanInProgress;
    doc["manualScanPending"] = status.manualScanPending;
    doc["savedAddressValid"] = status.savedAddressValid;
    doc["savedAddress"] = status.savedAddressValid ? String(obdRuntime->getSavedAddress()) : "";
    doc["connectAttempts"] = status.connectAttempts;
    doc["connectSuccesses"] = status.connectSuccesses;
    doc["connectFailures"] = status.connectFailures;
    doc["securityRepairs"] = status.securityRepairs;
    doc["initRetries"] = status.initRetries;
    doc["pollCount"] = status.pollCount;
    doc["pollErrors"] = status.pollErrors;
    doc["staleSpeedCount"] = status.staleSpeedCount;
    doc["consecutiveErrors"] = status.consecutiveErrors;
    doc["bufferOverflows"] = status.bufferOverflows;
    doc["commandInFlight"] = commandKindName(status.commandInFlight);
    doc["commandInFlightRaw"] = static_cast<int>(status.commandInFlight);
    doc["lastConnectStartMs"] = status.lastConnectStartMs;
    doc["lastConnectSuccessMs"] = status.lastConnectSuccessMs;
    doc["lastFailureMs"] = status.lastFailureMs;
    doc["lastBleError"] = status.lastBleError;
    doc["lastSecurityError"] = status.lastSecurityError;
    doc["lastFailureRaw"] = static_cast<int>(status.lastFailure);
    doc["state"] = static_cast<int>(status.state);
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiDevicesList(WebServer& server, ObdRuntimeModule* obdRuntime, SettingsManager& settings,
                          const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);

    WifiJson::Document doc;
    JsonArray arr = doc["devices"].to<JsonArray>();

    const V1Settings& s = settings.get();
    const String address = normalizeObdDeviceAddress(s.obdSavedAddress);
    if (address.length() > 0) {
        JsonObject obj = arr.add<JsonObject>();
        obj["address"] = address;
        obj["name"] = s.obdSavedName;
        obj["connected"] = !runtime.maintenanceBootActive && obdRuntime && obdRuntime->snapshot(millis()).connected;
        obj["active"] = true;
    }

    doc["count"] = arr.size();
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiDeviceNameSave(WebServer& server, SettingsManager& settings, const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);
    if (runtime.checkRateLimit && !runtime.checkRateLimit(runtime.ctx))
        return;

    if (!server.hasArg("address")) {
        server.send(400, "application/json", "{\"error\":\"Missing address\"}");
        return;
    }

    const String requestedAddress = normalizeObdDeviceAddress(server.arg("address"));
    const String savedAddress = normalizeObdDeviceAddress(settings.get().obdSavedAddress);
    if (requestedAddress.length() == 0 || savedAddress.length() == 0 ||
        !requestedAddress.equalsIgnoreCase(savedAddress)) {
        server.send(404, "application/json", "{\"error\":\"Saved OBD device not found\"}");
        return;
    }

    ObdSettingsUpdate update;
    update.hasSavedName = true;
    update.savedName = sanitizeObdDeviceName(server.hasArg("name") ? server.arg("name") : "");
    settings.applyObdSettingsUpdate(update, runtime.maintenanceBootActive
                                                ? SettingsPersistMode::Immediate
                                                : SettingsPersistMode::ImmediateNvsDeferredBackup);

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiScan(WebServer& server, ObdRuntimeModule* obdRuntime, const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);
    if (runtime.checkRateLimit && !runtime.checkRateLimit(runtime.ctx))
        return;
    if (runtime.maintenanceBootActive) {
        sendMaintenanceModeError(server);
        return;
    }
    if (!obdRuntime) {
        sendRuntimeUnavailableError(server);
        return;
    }
    if (!obdRuntime->isEnabled()) {
        WifiJson::Document doc;
        doc["success"] = false;
        doc["message"] = "OBD is disabled";
        WifiApiResponse::sendJsonDocument(server, 409, doc);
        return;
    }
    if (!obdRuntime->requestManualPairScan(millis())) {
        WifiJson::Document doc;
        doc["success"] = false;
        doc["message"] = "OBD scan already requested or in progress";
        WifiApiResponse::sendJsonDocument(server, 409, doc);
        return;
    }

    const ObdRuntimeStatus status = obdRuntime->snapshot(millis());
    WifiJson::Document doc;
    doc["success"] = true;
    doc["requested"] = true;
    doc["scanInProgress"] = status.scanInProgress;
    doc["message"] = status.scanInProgress ? "OBD scan already running" : "OBD scan requested";
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiForget(WebServer& server, ObdRuntimeModule* obdRuntime, SettingsManager& settings,
                     const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);
    if (runtime.checkRateLimit && !runtime.checkRateLimit(runtime.ctx))
        return;
    if (!runtime.maintenanceBootActive && !obdRuntime) {
        sendRuntimeUnavailableError(server);
        return;
    }
    if (!runtime.maintenanceBootActive) {
        obdRuntime->forgetDevice();
    }
    ObdSettingsUpdate update;
    update.hasSavedAddress = true;
    update.savedAddress = "";
    update.hasSavedName = true;
    update.savedName = "";
    update.hasSavedAddrType = true;
    update.savedAddrType = 0;
    settings.applyObdSettingsUpdate(update, runtime.maintenanceBootActive
                                                ? SettingsPersistMode::Immediate
                                                : SettingsPersistMode::ImmediateNvsDeferredBackup);
    WifiJson::Document doc;
    doc["success"] = true;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiConfig(WebServer& server, ObdRuntimeModule* obdRuntime, SettingsManager& settings,
                     const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);
    if (runtime.checkRateLimit && !runtime.checkRateLimit(runtime.ctx))
        return;

    if (!runtime.maintenanceBootActive && !obdRuntime) {
        sendRuntimeUnavailableError(server);
        return;
    }

    if (!server.hasArg("plain") || server.arg("plain").length() == 0) {
        WifiJson::Document errDoc;
        WifiApiResponse::setErrorAndMessage(errDoc, "Missing JSON body");
        WifiApiResponse::sendJsonDocument(server, 400, errDoc);
        return;
    }

    const String requestBody = server.arg("plain");
    WifiJson::Document body;
    DeserializationError err = deserializeJson(body, requestBody);
    if (err) {
        WifiJson::Document errDoc;
        WifiApiResponse::setErrorAndMessage(errDoc, "Invalid JSON");
        WifiApiResponse::sendJsonDocument(server, 400, errDoc);
        return;
    }

    ObdSettingsUpdate update;

    if (!readOptionalBool(server, body, "enabled", update.hasEnabled, update.enabled)) {
        return;
    }

    int rssi = 0;
    bool hasRssi = false;
    if (!readOptionalInt(server, body, "minRssi", hasRssi, rssi)) {
        return;
    }
    if (hasRssi) {
        update.hasMinRssi = true;
        update.minRssi = static_cast<int8_t>(std::max(-90, std::min(rssi, -40)));
    }

    struct DurationField {
        const char* key;
        bool* has;
        uint32_t* value;
    };
    const DurationField durationFields[] = {
        {"obdScanWindowMs", &update.hasObdScanWindowMs, &update.obdScanWindowMs},
        {"obdRetryIntervalMs", &update.hasObdRetryIntervalMs, &update.obdRetryIntervalMs},
        {"proxyOpenWindowMs", &update.hasProxyOpenWindowMs, &update.proxyOpenWindowMs},
        {"wifiOpenTimeoutMs", &update.hasWifiOpenTimeoutMs, &update.wifiOpenTimeoutMs},
        {"v1SettleQuietMs", &update.hasV1SettleQuietMs, &update.v1SettleQuietMs},
        {"v1SettleFallbackMs", &update.hasV1SettleFallbackMs, &update.v1SettleFallbackMs},
        {"cycleTeardownAckTimeoutMs", &update.hasCycleTeardownAckTimeoutMs, &update.cycleTeardownAckTimeoutMs},
    };
    for (const DurationField& field : durationFields) {
        int raw = 0;
        bool hasField = false;
        if (!readOptionalInt(server, body, field.key, hasField, raw)) {
            return;
        }
        if (hasField) {
            *field.has = true;
            *field.value = static_cast<uint32_t>(std::max(0, raw));
        }
    }

    const bool changed = settings.applyObdSettingsUpdate(update, runtime.maintenanceBootActive
                                                                     ? SettingsPersistMode::Immediate
                                                                     : SettingsPersistMode::ImmediateNvsDeferredBackup);
    if (changed && !runtime.maintenanceBootActive && runtime.syncAfterConfigChange) {
        runtime.syncAfterConfigChange(runtime.ctx);
    }

    WifiJson::Document doc;
    doc["success"] = true;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

} // namespace ObdApiService
