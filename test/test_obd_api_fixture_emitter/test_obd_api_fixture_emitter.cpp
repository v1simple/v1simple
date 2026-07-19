#include <unity.h>

#include <ArduinoJson.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../mocks/settings.h"
#include "../../src/modules/obd/obd_runtime_module.h"

ObdRuntimeStatus ObdRuntimeModule::snapshot(uint32_t /*nowMs*/) const {
    return ObdRuntimeStatus{};
}

bool ObdRuntimeModule::requestManualPairScan(uint32_t /*nowMs*/) {
    return false;
}

void ObdRuntimeModule::forgetDevice() {}

#include "../../src/modules/obd/obd_api_service.cpp"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {

constexpr const char* kFixtureMarker = "V1_WIFI_API_FIXTURE=";

struct CapturedResponse {
    const char* scenario = nullptr;
    const char* route = nullptr;
    int status = 0;
    std::string contentType;
    std::string body;
};

ObdApiService::Runtime makeRuntime() {
    ObdApiService::Runtime runtime;
    runtime.checkRateLimit = [](void* /*ctx*/) { return true; };
    runtime.maintenanceBootActive = true;
    return runtime;
}

void captureResponse(std::vector<CapturedResponse>& captures, const char* scenarioName, const char* routeKey,
                     const WebServer& server) {
    TEST_ASSERT_GREATER_OR_EQUAL_INT(100, server.lastStatusCode);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, server.lastContentType.length(), routeKey);

    JsonDocument body;
    const DeserializationError error = deserializeJson(body, server.lastBody.c_str());
    TEST_ASSERT_FALSE_MESSAGE(static_cast<bool>(error), server.lastBody.c_str());

    captures.push_back(CapturedResponse{scenarioName, routeKey, server.lastStatusCode, server.lastContentType.c_str(),
                                        server.lastBody.c_str()});
}

void captureConfig(std::vector<CapturedResponse>& captures, SettingsManager& settings) {
    WebServer server(80);
    ObdApiService::handleApiConfigGet(server, settings, makeRuntime());
    captureResponse(captures, "obd_config_invalid_value", "GET /api/obd/config", server);
}

void emitFixture() {
    std::vector<CapturedResponse> captures;
    SettingsManager settings;
    settings.settings.obdEnabled = true;
    settings.settings.obdMinRssi = -80;

    captureConfig(captures, settings);

    WebServer saveServer(80);
    saveServer.setArg("plain", "{\"minRssi\":-10}");
    ObdApiService::handleApiConfig(saveServer, nullptr, settings, makeRuntime());
    captureResponse(captures, "obd_config_invalid_value", "POST /api/obd/config", saveServer);

    captureConfig(captures, settings);

    TEST_ASSERT_EQUAL_INT8(-40, settings.settings.obdMinRssi);

    SettingsManager maintenanceSettings;
    maintenanceSettings.settings.obdEnabled = true;
    maintenanceSettings.settings.obdSavedAddress = "A4:C1:38:00:11:22";
    maintenanceSettings.settings.obdSavedName = "Truck Adapter";

    WebServer devicesServer(80);
    ObdApiService::handleApiDevicesList(devicesServer, nullptr, maintenanceSettings, makeRuntime());
    captureResponse(captures, "obd_maintenance_routes", "GET /api/obd/devices", devicesServer);

    WebServer statusServer(80);
    ObdApiService::handleApiStatus(statusServer, nullptr, makeRuntime());
    captureResponse(captures, "obd_maintenance_routes", "GET /api/obd/status", statusServer);

    WebServer nameSaveServer(80);
    nameSaveServer.setArg("address", "A4:C1:38:00:11:22");
    nameSaveServer.setArg("name", "Family Car");
    ObdApiService::handleApiDeviceNameSave(nameSaveServer, maintenanceSettings, makeRuntime());
    captureResponse(captures, "obd_maintenance_routes", "POST /api/obd/devices/name", nameSaveServer);

    WebServer scanServer(80);
    ObdApiService::handleApiScan(scanServer, nullptr, makeRuntime());
    captureResponse(captures, "obd_maintenance_routes", "POST /api/obd/scan", scanServer);

    WebServer forgetServer(80);
    ObdApiService::handleApiForget(forgetServer, nullptr, maintenanceSettings, makeRuntime());
    captureResponse(captures, "obd_maintenance_routes", "POST /api/obd/forget", forgetServer);

    TEST_ASSERT_EQUAL_STRING("", maintenanceSettings.settings.obdSavedAddress.c_str());
    TEST_ASSERT_EQUAL_UINT32(8, captures.size());

    std::ostringstream outputStream;
    outputStream << "{\"schemaVersion\":1,\"captures\":[";
    for (size_t index = 0; index < captures.size(); ++index) {
        const CapturedResponse& capture = captures[index];
        if (index > 0) {
            outputStream << ',';
        }
        outputStream << "{\"scenario\":\"" << capture.scenario << "\",\"route\":\"" << capture.route
                     << "\",\"status\":" << capture.status << ",\"contentType\":\"" << capture.contentType
                     << "\",\"body\":" << capture.body << '}';
    }
    outputStream << "]}";
    const std::string output = outputStream.str();
    const char* outputPath = std::getenv("V1_WIFI_API_FIXTURE_OUTPUT");
    if (outputPath && outputPath[0] != '\0') {
        std::ofstream outputFile(outputPath, std::ios::binary | std::ios::trunc);
        TEST_ASSERT_TRUE_MESSAGE(outputFile.is_open(), outputPath);
        outputFile << output;
        outputFile.close();
        TEST_ASSERT_TRUE_MESSAGE(outputFile.good(), outputPath);
        return;
    }

    const std::string fixtureMessage = std::string(kFixtureMarker) + output;
    TEST_MESSAGE(fixtureMessage.c_str());
}

} // namespace

void setUp() {}
void tearDown() {}

void test_real_obd_handler_responses_are_emitted_as_json_fixture() {
    emitFixture();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_real_obd_handler_responses_are_emitted_as_json_fixture);
    return UNITY_END();
}
