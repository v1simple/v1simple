#include "obd_api_service.h"

#include <algorithm>

#include <ArduinoJson.h>

#include "modules/obd/obd_runtime_module.h"
#include "modules/wifi/wifi_api_response.h"
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
    JsonDocument doc;
    doc["error"] = "maintenance_mode";
    doc["message"] = "OBD runtime endpoints are not available in maintenance mode";
    WifiApiResponse::sendJsonDocument(server, 409, doc);
}

} // namespace

void handleApiConfigGet(WebServer& server, SettingsManager& settings, const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);
    const V1Settings& s = settings.get();
    JsonDocument doc;
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

void handleApiStatus(WebServer& server, ObdRuntimeModule& obdRuntime, const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);
    if (runtime.maintenanceBootActive) {
        sendMaintenanceModeError(server);
        return;
    }
    ObdRuntimeStatus status = obdRuntime.snapshot(millis());
    JsonDocument doc;
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
    doc["savedAddress"] = status.savedAddressValid ? String(obdRuntime.getSavedAddress()) : "";
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

void handleApiDevicesList(WebServer& server, ObdRuntimeModule& obdRuntime, SettingsManager& settings,
                          const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);

    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();

    const V1Settings& s = settings.get();
    const String address = normalizeObdDeviceAddress(s.obdSavedAddress);
    if (address.length() > 0) {
        JsonObject obj = arr.add<JsonObject>();
        obj["address"] = address;
        obj["name"] = s.obdSavedName;
        obj["connected"] = obdRuntime.snapshot(millis()).connected;
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

void handleApiScan(WebServer& server, ObdRuntimeModule& obdRuntime, const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);
    if (runtime.checkRateLimit && !runtime.checkRateLimit(runtime.ctx))
        return;
    if (runtime.maintenanceBootActive) {
        sendMaintenanceModeError(server);
        return;
    }
    if (!obdRuntime.isEnabled()) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "OBD is disabled";
        WifiApiResponse::sendJsonDocument(server, 409, doc);
        return;
    }
    if (!obdRuntime.requestManualPairScan(millis())) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "OBD scan already requested or in progress";
        WifiApiResponse::sendJsonDocument(server, 409, doc);
        return;
    }

    const ObdRuntimeStatus status = obdRuntime.snapshot(millis());
    JsonDocument doc;
    doc["success"] = true;
    doc["requested"] = true;
    doc["scanInProgress"] = status.scanInProgress;
    doc["message"] = status.scanInProgress ? "OBD scan already running" : "OBD scan requested";
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiForget(WebServer& server, ObdRuntimeModule& obdRuntime, SettingsManager& settings,
                     const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);
    if (runtime.checkRateLimit && !runtime.checkRateLimit(runtime.ctx))
        return;
    if (!runtime.maintenanceBootActive) {
        obdRuntime.forgetDevice();
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
    JsonDocument doc;
    doc["success"] = true;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiConfig(WebServer& server, ObdRuntimeModule& obdRuntime, SettingsManager& settings,
                     const Runtime& runtime) {
    if (runtime.markUiActivity)
        runtime.markUiActivity(runtime.ctx);
    if (runtime.checkRateLimit && !runtime.checkRateLimit(runtime.ctx))
        return;

    if (!server.hasArg("plain") || server.arg("plain").length() == 0) {
        JsonDocument errDoc;
        WifiApiResponse::setErrorAndMessage(errDoc, "Missing JSON body");
        WifiApiResponse::sendJsonDocument(server, 400, errDoc);
        return;
    }

    const String requestBody = server.arg("plain");
    JsonDocument body;
    DeserializationError err = deserializeJson(body, requestBody);
    if (err) {
        JsonDocument errDoc;
        WifiApiResponse::setErrorAndMessage(errDoc, "Invalid JSON");
        WifiApiResponse::sendJsonDocument(server, 400, errDoc);
        return;
    }

    ObdSettingsUpdate update;

    if (body["enabled"].is<bool>()) {
        update.hasEnabled = true;
        update.enabled = body["enabled"].as<bool>();
    }
    if (!body["minRssi"].isNull()) {
        int rssi = body["minRssi"].as<int>();
        rssi = std::max(-90, std::min(rssi, -40));
        update.hasMinRssi = true;
        update.minRssi = static_cast<int8_t>(rssi);
    }
    if (body["obdScanWindowMs"].is<int>()) {
        update.hasObdScanWindowMs = true;
        update.obdScanWindowMs = static_cast<uint32_t>(std::max(0, body["obdScanWindowMs"].as<int>()));
    }
    if (body["obdRetryIntervalMs"].is<int>()) {
        update.hasObdRetryIntervalMs = true;
        update.obdRetryIntervalMs = static_cast<uint32_t>(std::max(0, body["obdRetryIntervalMs"].as<int>()));
    }
    if (body["proxyOpenWindowMs"].is<int>()) {
        update.hasProxyOpenWindowMs = true;
        update.proxyOpenWindowMs = static_cast<uint32_t>(std::max(0, body["proxyOpenWindowMs"].as<int>()));
    }
    if (body["wifiOpenTimeoutMs"].is<int>()) {
        update.hasWifiOpenTimeoutMs = true;
        update.wifiOpenTimeoutMs = static_cast<uint32_t>(std::max(0, body["wifiOpenTimeoutMs"].as<int>()));
    }
    if (body["v1SettleQuietMs"].is<int>()) {
        update.hasV1SettleQuietMs = true;
        update.v1SettleQuietMs = static_cast<uint32_t>(std::max(0, body["v1SettleQuietMs"].as<int>()));
    }
    if (body["v1SettleFallbackMs"].is<int>()) {
        update.hasV1SettleFallbackMs = true;
        update.v1SettleFallbackMs = static_cast<uint32_t>(std::max(0, body["v1SettleFallbackMs"].as<int>()));
    }
    if (body["cycleTeardownAckTimeoutMs"].is<int>()) {
        update.hasCycleTeardownAckTimeoutMs = true;
        update.cycleTeardownAckTimeoutMs =
            static_cast<uint32_t>(std::max(0, body["cycleTeardownAckTimeoutMs"].as<int>()));
    }

    const bool changed = settings.applyObdSettingsUpdate(update, runtime.maintenanceBootActive
                                                                     ? SettingsPersistMode::Immediate
                                                                     : SettingsPersistMode::ImmediateNvsDeferredBackup);
    if (changed && runtime.syncAfterConfigChange) {
        runtime.syncAfterConfigChange(runtime.ctx);
    }

    JsonDocument doc;
    doc["success"] = true;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

} // namespace ObdApiService
