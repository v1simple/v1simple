#include "wifi_v1_devices_api_service.h"

#include <ArduinoJson.h>

#include "wifi_api_response.h"
#include "wifi_json_document.h"

namespace WifiV1DevicesApiService {

void handleApiDevicesList(WebServer& server, const Runtime& runtime) {
    WifiJson::Document doc;
    JsonArray arr = doc["devices"].to<JsonArray>();

    std::vector<DeviceInfo> devices;
    if (runtime.listDevices) {
        devices = runtime.listDevices(runtime.listDevicesCtx);
    }

    for (const auto& device : devices) {
        JsonObject obj = arr.add<JsonObject>();
        obj["address"] = device.address;
        obj["name"] = device.name;
        obj["defaultProfile"] = device.defaultProfile;
        obj["connected"] = device.connected;
    }

    doc["count"] = devices.size();
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiDeviceNameSave(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                             void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;

    if (!server.hasArg("address")) {
        server.send(400, "application/json", "{\"error\":\"Missing address\"}");
        return;
    }

    if (!runtime.setDeviceName) {
        server.send(500, "application/json", "{\"error\":\"Device store unavailable\"}");
        return;
    }

    String address = server.arg("address");
    String name = server.hasArg("name") ? server.arg("name") : "";

    if (!runtime.setDeviceName(address, name, runtime.setDeviceNameCtx)) {
        server.send(400, "application/json", "{\"error\":\"Invalid address or write failed\"}");
        return;
    }

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiDeviceProfileSave(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                                void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;

    if (!server.hasArg("address") || !server.hasArg("profile")) {
        server.send(400, "application/json", "{\"error\":\"Missing address or profile\"}");
        return;
    }

    if (!runtime.setDeviceDefaultProfile) {
        server.send(500, "application/json", "{\"error\":\"Device store unavailable\"}");
        return;
    }

    String address = server.arg("address");
    int profile = server.arg("profile").toInt();
    if (profile < 0 || profile > 3) {
        server.send(400, "application/json", "{\"error\":\"Invalid profile\"}");
        return;
    }

    if (!runtime.setDeviceDefaultProfile(address, static_cast<uint8_t>(profile), runtime.setDeviceDefaultProfileCtx)) {
        server.send(400, "application/json", "{\"error\":\"Invalid address or write failed\"}");
        return;
    }

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiDeviceDelete(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                           void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;

    if (!server.hasArg("address")) {
        server.send(400, "application/json", "{\"error\":\"Missing address\"}");
        return;
    }

    if (!runtime.deleteDevice) {
        server.send(500, "application/json", "{\"error\":\"Device store unavailable\"}");
        return;
    }

    String address = server.arg("address");
    if (!runtime.deleteDevice(address, runtime.deleteDeviceCtx)) {
        server.send(400, "application/json", "{\"error\":\"Invalid address or write failed\"}");
        return;
    }

    server.send(200, "application/json", "{\"success\":true}");
}

} // namespace WifiV1DevicesApiService
