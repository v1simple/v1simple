#include "wifi_client_api_service.h"

#include <ArduinoJson.h>
#include <algorithm>

#include "wifi_api_response.h"
#include "wifi_json_document.h"

namespace WifiClientApiService {

namespace {

static void sendStatus(WebServer& server, const StatusPayload& payload) {
    WifiJson::Document doc;
    doc["enabled"] = payload.enabled;
    doc["savedSSID"] = payload.savedSsid;
    doc["state"] = payload.state;

    if (payload.includeConnectedFields) {
        doc["connectedSSID"] = payload.connectedSsid;
        if (payload.connectedSlotIndex >= 0) {
            doc["connectedSlotIndex"] = payload.connectedSlotIndex;
        }
        doc["ip"] = payload.ip;
        doc["rssi"] = payload.rssi;
    }

    doc["scanRunning"] = payload.scanRunning;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

static void sendScanInProgress(WebServer& server) {
    server.send(200, "application/json", "{\"scanning\":true,\"networks\":[]}");
}

static void sendScanIdle(WebServer& server) {
    server.send(200, "application/json", "{\"scanning\":false,\"networks\":[]}");
}

static void sendScanResults(WebServer& server, const std::vector<ScannedNetworkPayload>& networks) {
    WifiJson::Document doc;
    doc["scanning"] = false;
    JsonArray arr = doc["networks"].to<JsonArray>();

    for (const auto& net : networks) {
        JsonObject obj = arr.add<JsonObject>();
        obj["ssid"] = net.ssid;
        obj["rssi"] = net.rssi;
        obj["secure"] = net.secure;
    }

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

static void sendScanStartFailed(WebServer& server) {
    server.send(500, "application/json", "{\"success\":false,\"message\":\"Failed to start scan\"}");
}

static void sendRuntimeUnavailable(WebServer& server) {
    server.send(500, "application/json", "{\"success\":false,\"message\":\"Runtime unavailable\"}");
}

static void sendMaintenanceRequired(WebServer& server) {
    WifiJson::Document doc;
    doc["success"] = false;
    doc["error"] = "maintenance_required";
    doc["message"] = "WiFi network management is available only in maintenance mode";
    WifiApiResponse::sendJsonDocument(server, 409, doc);
}

static bool parseEnableRequest(WebServer& server, bool& enabledOut) {
    enabledOut = false;
    WifiJson::Document doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain").c_str());
    if (err || !doc["enabled"].is<bool>()) {
        return false;
    }
    enabledOut = doc["enabled"].as<bool>();
    return true;
}

static void sendEnableParseError(WebServer& server) {
    WifiJson::Document doc;
    doc["success"] = false;
    WifiApiResponse::setErrorAndMessage(doc, "Missing enabled field");
    WifiApiResponse::sendJsonDocument(server, 400, doc);
}

static void sendEnableResult(WebServer& server, bool enabled) {
    if (enabled) {
        server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi client enabled\"}");
        return;
    }
    server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi client disabled\"}");
}

static void sendEnableConnectFailed(WebServer& server) {
    server.send(500, "application/json", "{\"success\":false,\"message\":\"Failed to start connection\"}");
}

static void sendDisconnected(WebServer& server) {
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Disconnected\"}");
}

static void sendForgotten(WebServer& server) {
    server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi credentials forgotten\"}");
}

static void sendSavedNetworks(WebServer& server, const std::vector<SavedNetworkSlotPayload>& slots) {
    WifiJson::Document doc;
    JsonArray arr = doc["slots"].to<JsonArray>();
    for (const SavedNetworkSlotPayload& slot : slots) {
        JsonObject obj = arr.add<JsonObject>();
        obj["index"] = slot.index;
        obj["ssid"] = slot.ssid;
        obj["label"] = slot.label;
        obj["priority"] = slot.priority;
        obj["hasPassword"] = slot.hasPassword;
        obj["lastConnectedAtSec"] = slot.lastConnectedAtSec;
        obj["configured"] = slot.configured;
    }
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

static bool parseIndexValue(JsonVariantConst value, size_t& indexOut) {
    if (!value.is<int>()) {
        return false;
    }
    const int parsed = value.as<int>();
    if (parsed < 0) {
        return false;
    }
    indexOut = static_cast<size_t>(parsed);
    return true;
}

static bool parseNetworksSaveRequest(WebServer& server, SavedNetworkUpsertPayload& request,
                                     const char*& errorMessageOut) {
    request = SavedNetworkUpsertPayload();
    errorMessageOut = nullptr;

    if (!server.hasArg("plain")) {
        errorMessageOut = "Missing request body";
        return false;
    }

    WifiJson::Document doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain").c_str());
    if (error) {
        errorMessageOut = "Invalid JSON";
        return false;
    }

    if (!doc["index"].isNull()) {
        request.hasIndex = true;
        if (!parseIndexValue(doc["index"], request.index)) {
            errorMessageOut = "Invalid slot index";
            return false;
        }
    }

    request.ssid = doc["ssid"] | "";
    if (request.ssid.length() == 0) {
        errorMessageOut = "SSID required";
        return false;
    }

    if (doc["password"].is<const char*>()) {
        request.hasPassword = true;
        request.password = String(doc["password"] | "");
    }

    if (doc["label"].is<const char*>()) {
        request.hasLabel = true;
        request.label = String(doc["label"] | "");
    }

    if (doc["priority"].is<int>()) {
        const int parsedPriority = doc["priority"].as<int>();
        if (parsedPriority < 0 || parsedPriority > 255) {
            errorMessageOut = "Invalid priority";
            return false;
        }
        request.hasPriority = true;
        request.priority = static_cast<uint8_t>(parsedPriority);
    }

    return true;
}

static bool parseSlotIndexRequest(WebServer& server, size_t& indexOut, const char*& errorMessageOut) {
    indexOut = 0;
    errorMessageOut = nullptr;
    if (!server.hasArg("plain")) {
        errorMessageOut = "Missing request body";
        return false;
    }

    WifiJson::Document doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain").c_str());
    if (error) {
        errorMessageOut = "Invalid JSON";
        return false;
    }

    if (!parseIndexValue(doc["index"], indexOut)) {
        errorMessageOut = "Invalid slot index";
        return false;
    }
    return true;
}

static void sendRequestParseError(WebServer& server, const char* message) {
    WifiJson::Document doc;
    doc["success"] = false;
    WifiApiResponse::setErrorAndMessage(doc, message ? message : "Invalid request");
    WifiApiResponse::sendJsonDocument(server, 400, doc);
}

static void sendNetworkSaved(WebServer& server, size_t index) {
    WifiJson::Document doc;
    doc["success"] = true;
    doc["index"] = index;
    doc["message"] = "WiFi network saved";
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

static void sendNetworkSaveFailed(WebServer& server) {
    WifiJson::Document doc;
    doc["success"] = false;
    doc["error"] = "network_save_failed";
    doc["message"] = "Failed to save WiFi network";
    WifiApiResponse::sendJsonDocument(server, 409, doc);
}

static void sendNetworkDeleted(WebServer& server, size_t index) {
    WifiJson::Document doc;
    doc["success"] = true;
    doc["index"] = index;
    doc["message"] = "WiFi network deleted";
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

static void sendNetworkDeleteFailed(WebServer& server) {
    WifiJson::Document doc;
    doc["success"] = false;
    doc["error"] = "network_delete_failed";
    doc["message"] = "Failed to delete WiFi network";
    WifiApiResponse::sendJsonDocument(server, 404, doc);
}

static void sendNetworkTestStarted(WebServer& server, size_t index) {
    WifiJson::Document doc;
    doc["success"] = true;
    doc["index"] = index;
    doc["message"] = "Connecting...";
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

static void sendNetworkTestFailed(WebServer& server) {
    WifiJson::Document doc;
    doc["success"] = false;
    doc["error"] = "network_test_failed";
    doc["message"] = "Failed to start saved network connection test";
    WifiApiResponse::sendJsonDocument(server, 404, doc);
}

} // namespace

static void handleStatusImpl(WebServer& server, const Runtime& runtime) {
    if (!runtime.isEnabled || !runtime.getSavedSsid || !runtime.getStateName || !runtime.isScanRunning ||
        !runtime.isConnected) {
        sendRuntimeUnavailable(server);
        return;
    }

    StatusPayload payload;
    payload.enabled = runtime.isEnabled(runtime.isEnabledCtx);
    payload.savedSsid = runtime.getSavedSsid(runtime.getSavedSsidCtx);
    payload.state = runtime.getStateName(runtime.getStateNameCtx);
    payload.scanRunning = runtime.isScanRunning(runtime.isScanRunningCtx);

    if (runtime.isConnected(runtime.isConnectedCtx) && runtime.getConnectedNetwork) {
        const ConnectedNetworkPayload connected = runtime.getConnectedNetwork(runtime.getConnectedNetworkCtx);
        payload.includeConnectedFields = true;
        payload.connectedSsid = connected.ssid;
        payload.connectedSlotIndex = connected.connectedSlotIndex;
        payload.ip = connected.ip;
        payload.rssi = connected.rssi;
    }

    sendStatus(server, payload);
}

static void handleScanImpl(WebServer& server, const Runtime& runtime, bool startIfIdle) {
    if (!runtime.isScanRunning || !runtime.isScanInProgress || !runtime.hasCompletedScanResults ||
        !runtime.getScannedNetworks || (startIfIdle && !runtime.startScan)) {
        sendRuntimeUnavailable(server);
        return;
    }

    Serial.printf("[HTTP] %s /api/wifi/scan\n", startIfIdle ? "POST" : "GET");

    const bool activeUiScan = runtime.isScanRunning(runtime.isScanRunningCtx);
    if (activeUiScan) {
        if (runtime.isScanInProgress(runtime.isScanInProgressCtx)) {
            sendScanInProgress(server);
            return;
        }
        if (runtime.hasCompletedScanResults(runtime.hasCompletedScanResultsCtx)) {
            sendScanResults(server, runtime.getScannedNetworks(runtime.getScannedNetworksCtx));
            return;
        }
    }

    if (startIfIdle) {
        if (runtime.startScan(runtime.startScanCtx)) {
            sendScanInProgress(server);
            return;
        }
        sendScanStartFailed(server);
        return;
    }

    if (runtime.hasCompletedScanResults(runtime.hasCompletedScanResultsCtx)) {
        sendScanResults(server, runtime.getScannedNetworks(runtime.getScannedNetworksCtx));
        return;
    }

    sendScanIdle(server);
}

static void handleDisconnectImpl(WebServer& server, const Runtime& runtime) {
    if (!runtime.disconnectFromNetwork) {
        sendRuntimeUnavailable(server);
        return;
    }

    Serial.println("[HTTP] POST /api/wifi/disconnect");

    runtime.disconnectFromNetwork(runtime.disconnectFromNetworkCtx);
    sendDisconnected(server);
}

static void handleForgetImpl(WebServer& server, const Runtime& runtime) {
    if (!runtime.forgetClient) {
        sendRuntimeUnavailable(server);
        return;
    }

    Serial.println("[HTTP] POST /api/wifi/forget");
    runtime.forgetClient(runtime.forgetClientCtx);
    sendForgotten(server);
}

static void handleEnableImpl(WebServer& server, const Runtime& runtime) {
    if (!runtime.enableWithSavedNetwork || !runtime.disableClient) {
        sendRuntimeUnavailable(server);
        return;
    }

    bool enable = false;
    if (!parseEnableRequest(server, enable)) {
        sendEnableParseError(server);
        return;
    }

    Serial.printf("[HTTP] POST /api/wifi/enable: %s\n", enable ? "true" : "false");
    if (enable) {
        if (!runtime.enableWithSavedNetwork(runtime.enableWithSavedNetworkCtx)) {
            sendEnableConnectFailed(server);
            return;
        }
        sendEnableResult(server, true);
        return;
    }

    runtime.disableClient(runtime.disableClientCtx);
    sendEnableResult(server, false);
}

static void handleNetworksImpl(WebServer& server, const Runtime& runtime) {
    if (!runtime.maintenanceBootActive) {
        sendMaintenanceRequired(server);
        return;
    }
    if (!runtime.getSavedNetworks) {
        sendRuntimeUnavailable(server);
        return;
    }

    sendSavedNetworks(server, runtime.getSavedNetworks(runtime.getSavedNetworksCtx));
}

static void handleNetworksSaveImpl(WebServer& server, const Runtime& runtime) {
    if (!runtime.maintenanceBootActive) {
        sendMaintenanceRequired(server);
        return;
    }
    if (!runtime.upsertSavedNetwork) {
        sendRuntimeUnavailable(server);
        return;
    }

    SavedNetworkUpsertPayload request;
    const char* errorMessage = nullptr;
    if (!parseNetworksSaveRequest(server, request, errorMessage)) {
        sendRequestParseError(server, errorMessage);
        return;
    }

    size_t index = 0;
    if (!runtime.upsertSavedNetwork(request, index, runtime.upsertSavedNetworkCtx)) {
        sendNetworkSaveFailed(server);
        return;
    }

    sendNetworkSaved(server, index);
}

static void handleNetworksDeleteImpl(WebServer& server, const Runtime& runtime) {
    if (!runtime.maintenanceBootActive) {
        sendMaintenanceRequired(server);
        return;
    }
    if (!runtime.deleteSavedNetwork) {
        sendRuntimeUnavailable(server);
        return;
    }

    size_t index = 0;
    const char* errorMessage = nullptr;
    if (!parseSlotIndexRequest(server, index, errorMessage)) {
        sendRequestParseError(server, errorMessage);
        return;
    }

    if (!runtime.deleteSavedNetwork(index, runtime.deleteSavedNetworkCtx)) {
        sendNetworkDeleteFailed(server);
        return;
    }

    sendNetworkDeleted(server, index);
}

static void handleNetworksTestImpl(WebServer& server, const Runtime& runtime) {
    if (!runtime.maintenanceBootActive) {
        sendMaintenanceRequired(server);
        return;
    }
    if (!runtime.testSavedNetwork) {
        sendRuntimeUnavailable(server);
        return;
    }

    size_t index = 0;
    const char* errorMessage = nullptr;
    if (!parseSlotIndexRequest(server, index, errorMessage)) {
        sendRequestParseError(server, errorMessage);
        return;
    }

    if (!runtime.testSavedNetwork(index, runtime.testSavedNetworkCtx)) {
        sendNetworkTestFailed(server);
        return;
    }

    sendNetworkTestStarted(server, index);
}

void handleApiStatus(WebServer& server, const Runtime& runtime, void (*markUiActivity)(void* ctx),
                     void* uiActivityCtx) {
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleStatusImpl(server, runtime);
}

void handleApiScan(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                   void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleScanImpl(server, runtime, true);
}

void handleApiScanStatus(WebServer& server, const Runtime& runtime, void (*markUiActivity)(void* ctx),
                         void* uiActivityCtx) {
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleScanImpl(server, runtime, false);
}

void handleApiDisconnect(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                         void* rateLimitCtx, void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleDisconnectImpl(server, runtime);
}

void handleApiForget(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleForgetImpl(server, runtime);
}

void handleApiEnable(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleEnableImpl(server, runtime);
}

void handleApiNetworks(WebServer& server, const Runtime& runtime, void (*markUiActivity)(void* ctx),
                       void* uiActivityCtx) {
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleNetworksImpl(server, runtime);
}

void handleApiNetworksSave(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                           void* rateLimitCtx, void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleNetworksSaveImpl(server, runtime);
}

void handleApiNetworksDelete(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                             void* rateLimitCtx, void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleNetworksDeleteImpl(server, runtime);
}

void handleApiNetworksTest(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                           void* rateLimitCtx, void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleNetworksTestImpl(server, runtime);
}

} // namespace WifiClientApiService
