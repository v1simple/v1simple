#include "wifi_v1_devices_api_service.h"

#include <ArduinoJson.h>
#include <cstdlib>

#include "wifi_api_response.h"
#include "psram_json_document.h"
#include "json_exact_input.h"
#include "exact_urlencoded_form.h"

namespace WifiV1DevicesApiService {

namespace {

bool exactDeviceNameIsCanonical(const String& name) {
    if (name.length() > 32u ||
        !ExactJsonInput::validSemanticString(name.c_str(), name.length())) return false;
    for (size_t index = 0; index < name.length(); ++index) {
        if (static_cast<uint8_t>(name[index]) < 0x20u) return false;
    }
    return name.length() == 0 ||
           (static_cast<uint8_t>(name[0]) > static_cast<uint8_t>(' ') &&
            static_cast<uint8_t>(name[name.length() - 1u]) > static_cast<uint8_t>(' '));
}

void sendMutationResult(WebServer& server, V1DeviceMutationResult result) {
    switch (result.status) {
    case V1DeviceMutationStatus::FullyMirrored:
        server.send(200, "application/json", "{\"success\":true}");
        return;
    case V1DeviceMutationStatus::PrimaryCommittedMirrorPending:
        server.send(202, "application/json",
                    "{\"success\":true,\"mirrorSyncPending\":true}");
        return;
    case V1DeviceMutationStatus::DurablePending:
        server.send(202, "application/json",
                    "{\"success\":true,\"operationPending\":true}");
        return;
    case V1DeviceMutationStatus::Invalid:
        server.send(400, "application/json", "{\"error\":\"Invalid device request\"}");
        return;
    case V1DeviceMutationStatus::Busy:
        server.send(409, "application/json", "{\"error\":\"Another device deletion is pending\"}");
        return;
    case V1DeviceMutationStatus::Unavailable:
    case V1DeviceMutationStatus::NotCommitted:
        server.send(503, "application/json", "{\"error\":\"Device update not committed\"}");
        return;
    }
}

} // namespace

void handleApiDevicesList(WebServer& server, const Runtime& runtime) {
    std::vector<DeviceInfo> devices;
    if (!runtime.listDevices || !runtime.listDevices(devices, runtime.listDevicesCtx)) {
        server.send(503, "application/json", "{\"error\":\"Device catalog unavailable\"}");
        return;
    }

    PsramJson::Document doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    for (const auto& device : devices) {
        JsonObject obj = arr.add<JsonObject>();
        obj["address"] = device.address;
        obj["name"] = device.name;
        obj["defaultProfile"] = device.defaultProfile;
        obj["connected"] = device.connected;
    }

    doc["count"] = devices.size();
    if (doc.overflowed() || measureJson(doc) == 0) {
        server.send(503, "application/json", "{\"error\":\"Device catalog unavailable\"}");
        return;
    }
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiDeviceNameSaveBody(WebServer& server, const Runtime& runtime,
                                 const uint8_t* body, size_t bodySize,
                                 bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                                 const char* multipartBoundary, size_t multipartBoundarySize) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    const ExactUrlEncodedForm form = multipartBoundarySize == 0
        ? ExactUrlEncodedForm(body, bodySize)
        : ExactUrlEncodedForm(body, bodySize, multipartBoundary, multipartBoundarySize);
    static constexpr const char* ALLOWED[] = {"address", "name"};
    if (!form.valid() || !form.hasOnly(ALLOWED, sizeof(ALLOWED) / sizeof(ALLOWED[0])) ||
        !form.has("address")) {
        server.send(400, "application/json", "{\"error\":\"Invalid form body\"}");
        return;
    }
    if (!runtime.setDeviceName) {
        server.send(500, "application/json", "{\"error\":\"Device store unavailable\"}");
        return;
    }
    String address;
    String name;
    if (!form.read("address", address) ||
        (form.has("name") && !form.read("name", name, true))) {
        server.send(503, "application/json", "{\"error\":\"Device parameter unavailable\"}");
        return;
    }
    if (!exactDeviceNameIsCanonical(name)) {
        server.send(400, "application/json", "{\"error\":\"Invalid device name\"}");
        return;
    }
    sendMutationResult(server, runtime.setDeviceName(address, name, runtime.setDeviceNameCtx));
}

void handleApiDeviceProfileSaveBody(WebServer& server, const Runtime& runtime,
                                    const uint8_t* body, size_t bodySize,
                                    bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                                    const char* multipartBoundary, size_t multipartBoundarySize) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    const ExactUrlEncodedForm form = multipartBoundarySize == 0
        ? ExactUrlEncodedForm(body, bodySize)
        : ExactUrlEncodedForm(body, bodySize, multipartBoundary, multipartBoundarySize);
    static constexpr const char* ALLOWED[] = {"address", "profile"};
    if (!form.valid() || !form.hasOnly(ALLOWED, sizeof(ALLOWED) / sizeof(ALLOWED[0])) ||
        !form.has("address") || !form.has("profile")) {
        server.send(400, "application/json", "{\"error\":\"Invalid form body\"}");
        return;
    }
    if (!runtime.setDeviceDefaultProfile) {
        server.send(500, "application/json", "{\"error\":\"Device store unavailable\"}");
        return;
    }
    String address;
    String profileText;
    if (!form.read("address", address) || !form.read("profile", profileText)) {
        server.send(503, "application/json", "{\"error\":\"Device parameter unavailable\"}");
        return;
    }
    char* end = nullptr;
    const long profile = std::strtol(profileText.c_str(), &end, 10);
    if (!end || *end != '\0' || profile < 0 || profile > 3) {
        server.send(400, "application/json", "{\"error\":\"Invalid profile\"}");
        return;
    }
    sendMutationResult(server, runtime.setDeviceDefaultProfile(
                                   address, static_cast<uint8_t>(profile),
                                   runtime.setDeviceDefaultProfileCtx));
}

void handleApiDeviceDeleteBody(WebServer& server, const Runtime& runtime,
                               const uint8_t* body, size_t bodySize,
                               bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                               const char* multipartBoundary, size_t multipartBoundarySize) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    const ExactUrlEncodedForm form = multipartBoundarySize == 0
        ? ExactUrlEncodedForm(body, bodySize)
        : ExactUrlEncodedForm(body, bodySize, multipartBoundary, multipartBoundarySize);
    static constexpr const char* ALLOWED[] = {"address"};
    if (!form.valid() || !form.hasOnly(ALLOWED, sizeof(ALLOWED) / sizeof(ALLOWED[0])) ||
        !form.has("address")) {
        server.send(400, "application/json", "{\"error\":\"Invalid form body\"}");
        return;
    }
    if (!runtime.deleteDevice) {
        server.send(500, "application/json", "{\"error\":\"Device store unavailable\"}");
        return;
    }
    String address;
    if (!form.read("address", address)) {
        server.send(503, "application/json", "{\"error\":\"Device parameter unavailable\"}");
        return;
    }
    sendMutationResult(server, runtime.deleteDevice(address, runtime.deleteDeviceCtx));
}

} // namespace WifiV1DevicesApiService
