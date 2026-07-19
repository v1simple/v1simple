#include <unity.h>

#include <ArduinoJson.h>

#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../mocks/mock_heap_caps_state.h"
#include "../../src/modules/wifi/backup_api_service.h"
#include "../../src/modules/wifi/wifi_autopush_api_service.h"
#include "../../src/modules/wifi/wifi_client_api_service.h"
#include "../../src/modules/wifi/wifi_diagnostics_api_service.h"
#include "../../src/modules/wifi/wifi_settings_api_service.h"
#include "../../src/modules/wifi/wifi_status_api_service.h"
#include "../../src/modules/wifi/wifi_system_api_service.h"
#include "../../src/modules/wifi/wifi_v1_devices_api_service.h"
#include "../../src/modules/wifi/wifi_v1_profile_api_service.h"

namespace BackupPayloadBuilder {

bool isRecognizedBackupType(const char* type) {
    return type != nullptr &&
           (std::strcmp(type, "v1simple_backup") == 0 || std::strcmp(type, "v1simple_sd_backup") == 0);
}

} // namespace BackupPayloadBuilder

#include "../../src/modules/wifi/backup_snapshot_cache.cpp"
#include "../../src/modules/wifi/backup_api_service.cpp"
#include "../../src/modules/wifi/wifi_autopush_api_service.cpp"
#include "../../src/modules/wifi/wifi_client_api_service.cpp"
#include "../../src/modules/wifi/wifi_diagnostics_api_service.cpp"
#include "../../src/modules/wifi/wifi_settings_api_service.cpp"
#include "../../src/modules/wifi/wifi_status_api_service.cpp"
#include "../../src/modules/wifi/wifi_system_api_service.cpp"
#include "../../src/modules/wifi/wifi_v1_devices_api_service.cpp"
#include "../../src/modules/wifi/wifi_v1_profile_api_service.cpp"

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
    bool bodyIsJson = true;
};

void captureResponse(std::vector<CapturedResponse>& captures, const char* scenarioName, const char* routeKey,
                     const WebServer& server, bool bodyIsJson = true) {
    TEST_ASSERT_GREATER_OR_EQUAL_INT(100, server.lastStatusCode);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, server.lastContentType.length(), routeKey);
    if (bodyIsJson) {
        JsonDocument body;
        const DeserializationError error = deserializeJson(body, server.lastBody.c_str());
        TEST_ASSERT_FALSE_MESSAGE(static_cast<bool>(error), server.lastBody.c_str());
    }
    captures.push_back(CapturedResponse{scenarioName, routeKey, server.lastStatusCode, server.lastContentType.c_str(),
                                        server.lastBody.c_str(), bodyIsJson});
}

std::string jsonString(const std::string& value) {
    JsonDocument doc;
    doc.set(value);
    std::string encoded;
    serializeJson(doc, encoded);
    return encoded;
}

struct DeviceSettingsState {
    V1Settings settings;
    int applyCalls = 0;
};

WifiSettingsApiService::Runtime makeSettingsRuntime(DeviceSettingsState& state) {
    WifiSettingsApiService::Runtime runtime;
    runtime.ctx = &state;
    runtime.getSettings = [](void* ctx) -> const V1Settings& {
        return static_cast<DeviceSettingsState*>(ctx)->settings;
    };
    runtime.applySettingsUpdate = [](const DeviceSettingsUpdate& update, void* ctx) {
        auto* state = static_cast<DeviceSettingsState*>(ctx);
        state->applyCalls++;
        if (update.hasProxyName) {
            state->settings.proxyName = update.proxyName;
        }
    };
    runtime.checkRateLimit = [](void*) { return true; };
    return runtime;
}

struct WifiMutationState {
    size_t savedIndex = 1;
    int saveCalls = 0;
    int disconnectCalls = 0;
    int deleteCalls = 0;
    int testCalls = 0;
};

WifiClientApiService::Runtime makeWifiMutationRuntime(WifiMutationState& state) {
    WifiClientApiService::Runtime runtime;
    runtime.maintenanceBootActive = true;
    runtime.disconnectFromNetwork = [](void* ctx) { static_cast<WifiMutationState*>(ctx)->disconnectCalls++; };
    runtime.disconnectFromNetworkCtx = &state;
    runtime.upsertSavedNetwork = [](const WifiClientApiService::SavedNetworkUpsertPayload&, size_t& indexOut,
                                    void* ctx) {
        auto* state = static_cast<WifiMutationState*>(ctx);
        state->saveCalls++;
        indexOut = state->savedIndex;
        return true;
    };
    runtime.upsertSavedNetworkCtx = &state;
    runtime.deleteSavedNetwork = [](size_t, void* ctx) {
        static_cast<WifiMutationState*>(ctx)->deleteCalls++;
        return true;
    };
    runtime.deleteSavedNetworkCtx = &state;
    runtime.testSavedNetwork = [](size_t, void* ctx) {
        static_cast<WifiMutationState*>(ctx)->testCalls++;
        return true;
    };
    runtime.testSavedNetworkCtx = &state;
    return runtime;
}

struct BackupState {
    int buildCalls = 0;
    int backupCalls = 0;
};

BackupApiService::BackupRuntime makeBackupRuntime(BackupState& state) {
    BackupApiService::BackupRuntime runtime;
    runtime.ctx = &state;
    runtime.getBackupRevision = [](void*) { return 7u; };
    runtime.getCatalogRevision = [](void*) { return 3u; };
    runtime.buildDocument = [](JsonDocument& doc, uint32_t snapshotMs, void* ctx) {
        static_cast<BackupState*>(ctx)->buildCalls++;
        doc["_type"] = "v1simple_backup";
        doc["version"] = 1;
        doc["snapshotMs"] = snapshotMs;
        doc["profiles"].to<JsonArray>();
    };
    runtime.isStorageReady = [](void*) { return true; };
    runtime.isSDCard = [](void*) { return true; };
    runtime.backupToSD = [](void* ctx) {
        static_cast<BackupState*>(ctx)->backupCalls++;
        return true;
    };
    return runtime;
}

struct AutoPushState {
    WifiAutoPushApiService::SlotsSnapshot snapshot;
    int slotSaveCalls = 0;
    int activateCalls = 0;
    int pushCalls = 0;
};

WifiAutoPushApiService::Runtime makeAutoPushRuntime(AutoPushState& state) {
    WifiAutoPushApiService::Runtime runtime;
    runtime.loadSlotsSnapshot = [](WifiAutoPushApiService::SlotsSnapshot& snapshot, void* ctx) {
        snapshot = static_cast<AutoPushState*>(ctx)->snapshot;
    };
    runtime.loadSlotsSnapshotCtx = &state;
    runtime.applySlotUpdate = [](const WifiAutoPushApiService::SlotUpdateRequest&, void* ctx) {
        static_cast<AutoPushState*>(ctx)->slotSaveCalls++;
        return true;
    };
    runtime.applySlotUpdateCtx = &state;
    runtime.applyActivation = [](const WifiAutoPushApiService::ActivationRequest&, void* ctx) {
        static_cast<AutoPushState*>(ctx)->activateCalls++;
        return true;
    };
    runtime.applyActivationCtx = &state;
    runtime.queuePushNow = [](const WifiAutoPushApiService::PushNowRequest&, void* ctx) {
        static_cast<AutoPushState*>(ctx)->pushCalls++;
        return WifiAutoPushApiService::PushNowQueueResult::QUEUED;
    };
    runtime.queuePushNowCtx = &state;
    return runtime;
}

struct ProfileState {
    V1Settings settings;
    int saveCalls = 0;
    int deleteCalls = 0;
    int pullCalls = 0;
    int pushCalls = 0;
};

WifiV1ProfileApiService::Runtime makeProfileRuntime(ProfileState& state) {
    WifiV1ProfileApiService::Runtime runtime;
    runtime.listProfileNames = [](void*) { return std::vector<String>{"Daily Drive"}; };
    runtime.listProfileNamesCtx = &state;
    runtime.loadProfileSummary = [](const String& name, WifiV1ProfileApiService::ProfileSummary& summary, void*) {
        summary.name = name;
        summary.description = name == "Daily Drive" ? "Daily drive" : "";
        summary.displayOn = true;
        return true;
    };
    runtime.loadProfileSummaryCtx = &state;
    runtime.loadProfileJson = [](const String& name, String& json, void*) {
        if (name != "Commute") {
            return false;
        }
        json = "{\"name\":\"Commute\",\"description\":\"Daily drive\",\"displayOn\":true,"
               "\"settings\":{\"byte0\":1}}";
        return true;
    };
    runtime.loadProfileJsonCtx = &state;
    runtime.loadProfileSettings = [](const String&, uint8_t outBytes[6], bool& displayOn, void*) {
        for (uint8_t index = 0; index < 6; ++index) {
            outBytes[index] = static_cast<uint8_t>(index + 1);
        }
        displayOn = true;
        return true;
    };
    runtime.loadProfileSettingsCtx = &state;
    runtime.parseSettingsJson = [](const JsonObject& settings, uint8_t outBytes[6], void*) {
        std::memset(outBytes, 0xFF, 6);
        outBytes[0] = settings["byte0"] | 0;
        return true;
    };
    runtime.parseSettingsJsonCtx = &state;
    runtime.saveProfile = [](const String&, const String&, bool, const uint8_t[6], String&, void* ctx) {
        static_cast<ProfileState*>(ctx)->saveCalls++;
        return true;
    };
    runtime.saveProfileCtx = &state;
    runtime.deleteProfile = [](const String&, void* ctx) {
        static_cast<ProfileState*>(ctx)->deleteCalls++;
        return true;
    };
    runtime.deleteProfileCtx = &state;
    runtime.requestUserBytes = [](void* ctx) {
        static_cast<ProfileState*>(ctx)->pullCalls++;
        return true;
    };
    runtime.requestUserBytesCtx = &state;
    runtime.writeUserBytes = [](const uint8_t[6], void* ctx) {
        static_cast<ProfileState*>(ctx)->pushCalls++;
        return true;
    };
    runtime.writeUserBytesCtx = &state;
    runtime.getSettings = [](void* ctx) -> const V1Settings& { return static_cast<ProfileState*>(ctx)->settings; };
    runtime.getSettingsCtx = &state;
    runtime.setDisplayOn = [](bool, void*) {};
    runtime.setDisplayOnCtx = &state;
    runtime.hasCurrentSettings = [](void*) { return true; };
    runtime.hasCurrentSettingsCtx = &state;
    runtime.currentSettingsJson = [](void*) { return String("{\"xBand\":true}"); };
    runtime.currentSettingsJsonCtx = &state;
    runtime.v1Connected = [](void*) { return true; };
    runtime.v1ConnectedCtx = &state;
    runtime.backupToSd = [](void*) {};
    runtime.backupToSdCtx = &state;
    return runtime;
}

struct DeviceState {
    int nameCalls = 0;
    int profileCalls = 0;
    int deleteCalls = 0;
};

WifiV1DevicesApiService::Runtime makeDeviceRuntime(DeviceState& state) {
    WifiV1DevicesApiService::Runtime runtime;
    runtime.listDevices = [](void*) {
        return std::vector<WifiV1DevicesApiService::DeviceInfo>{
            {"AA:BB:CC:DD:EE:FF", "Daily Driver", 2, true},
        };
    };
    runtime.listDevicesCtx = &state;
    runtime.setDeviceName = [](const String&, const String&, void* ctx) {
        static_cast<DeviceState*>(ctx)->nameCalls++;
        return true;
    };
    runtime.setDeviceNameCtx = &state;
    runtime.setDeviceDefaultProfile = [](const String&, uint8_t, void* ctx) {
        static_cast<DeviceState*>(ctx)->profileCalls++;
        return true;
    };
    runtime.setDeviceDefaultProfileCtx = &state;
    runtime.deleteDevice = [](const String&, void* ctx) {
        static_cast<DeviceState*>(ctx)->deleteCalls++;
        return true;
    };
    runtime.deleteDeviceCtx = &state;
    return runtime;
}

void captureCoreRoutes(std::vector<CapturedResponse>& captures) {
    WifiStatusApiService::StatusJsonCache statusCache;
    unsigned long lastStatusTime = 0;
    unsigned long now = 1000;
    WifiStatusApiService::StatusRuntime statusRuntime;
    statusRuntime.mergeStatus2 = [](JsonObject object, void*) {
        object["maintenanceBoot"] = false;
        object["maintenanceBootUptimeMs"] = 0;
        object["maintenanceBootTimeoutMs"] = 600000;
    };
    WebServer statusServer(80);
    WifiStatusApiService::handleApiStatus(
        statusServer, statusRuntime, statusCache, lastStatusTime, 500,
        [](void* ctx) -> unsigned long { return *static_cast<unsigned long*>(ctx); }, &now, nullptr, nullptr);
    captureResponse(captures, "frontend_core_routes", "GET /api/status", statusServer);
    WifiStatusApiService::releaseStatusJsonCache(statusCache, lastStatusTime);

    DeviceSettingsState settings;
    settings.settings.proxyName = "Fixture Unit";
    WebServer settingsServer(80);
    settingsServer.setArg("proxy_name", "Fixture Unit Renamed");
    WifiSettingsApiService::handleApiDeviceSettingsSave(settingsServer, makeSettingsRuntime(settings));
    captureResponse(captures, "frontend_core_routes", "POST /api/device/settings", settingsServer);
    TEST_ASSERT_EQUAL_INT(1, settings.applyCalls);

    int rebootCalls = 0;
    WifiSystemApiService::RebootRuntime rebootRuntime;
    rebootRuntime.maintenanceBootActive = true;
    rebootRuntime.ctx = &rebootCalls;
    rebootRuntime.persistSettings = [](void* ctx) { ++*static_cast<int*>(ctx); };
    rebootRuntime.markCleanShutdown = [](void* ctx) { ++*static_cast<int*>(ctx); };
    rebootRuntime.restart = [](void* ctx) { ++*static_cast<int*>(ctx); };
    WebServer rebootServer(80);
    WifiSystemApiService::handleApiRebootNormal(rebootServer, rebootRuntime);
    captureResponse(captures, "frontend_core_routes", "POST /api/system/reboot-normal", rebootServer);
    TEST_ASSERT_EQUAL_INT(3, rebootCalls);
}

void captureWifiRoutes(std::vector<CapturedResponse>& captures) {
    WifiMutationState state;
    const WifiClientApiService::Runtime runtime = makeWifiMutationRuntime(state);

    WebServer saveServer(80);
    saveServer.setArg("plain", "{\"ssid\":\"BenchAP\",\"password\":\"fixture-pass\",\"priority\":1}");
    WifiClientApiService::handleApiNetworksSave(saveServer, runtime, nullptr, nullptr, nullptr, nullptr);
    captureResponse(captures, "wifi_network_mutations", "POST /api/wifi/networks", saveServer);

    WebServer disconnectServer(80);
    WifiClientApiService::handleApiDisconnect(disconnectServer, runtime, nullptr, nullptr, nullptr, nullptr);
    captureResponse(captures, "wifi_network_mutations", "POST /api/wifi/disconnect", disconnectServer);

    WebServer deleteServer(80);
    deleteServer.setArg("plain", "{\"index\":1}");
    WifiClientApiService::handleApiNetworksDelete(deleteServer, runtime, nullptr, nullptr, nullptr, nullptr);
    captureResponse(captures, "wifi_network_mutations", "POST /api/wifi/networks/delete", deleteServer);

    WebServer testServer(80);
    testServer.setArg("plain", "{\"index\":1}");
    WifiClientApiService::handleApiNetworksTest(testServer, runtime, nullptr, nullptr, nullptr, nullptr);
    captureResponse(captures, "wifi_network_mutations", "POST /api/wifi/networks/test", testServer);

    TEST_ASSERT_EQUAL_INT(1, state.saveCalls);
    TEST_ASSERT_EQUAL_INT(1, state.disconnectCalls);
    TEST_ASSERT_EQUAL_INT(1, state.deleteCalls);
    TEST_ASSERT_EQUAL_INT(1, state.testCalls);
}

void captureBackupRoutes(std::vector<CapturedResponse>& captures) {
    BackupState state;
    const BackupApiService::BackupRuntime runtime = makeBackupRuntime(state);
    BackupApiService::BackupSnapshotCache cache;
    uint32_t now = 4242;

    WebServer backupServer(80);
    BackupApiService::handleApiBackup(
        backupServer, cache, runtime, nullptr, nullptr, [](void* ctx) { return *static_cast<uint32_t*>(ctx); }, &now);
    captureResponse(captures, "backup_routes", "GET /api/settings/backup", backupServer);
    BackupApiService::releaseBackupSnapshotCache(cache);

    WebServer backupNowServer(80);
    BackupApiService::handleApiBackupNow(backupNowServer, runtime, nullptr, nullptr, nullptr, nullptr);
    captureResponse(captures, "backup_routes", "POST /api/settings/backup-now", backupNowServer);

    TEST_ASSERT_EQUAL_INT(1, state.buildCalls);
    TEST_ASSERT_EQUAL_INT(1, state.backupCalls);
}

void captureAutoPushRoutes(std::vector<CapturedResponse>& captures) {
    AutoPushState state;
    state.snapshot.enabled = true;
    state.snapshot.activeSlot = 1;
    state.snapshot.slots[0].name = "Default";
    state.snapshot.slots[0].profile = "Road Trip";
    state.snapshot.slots[0].mode = 2;
    state.snapshot.slots[0].volume = 6;
    state.snapshot.slots[0].muteVolume = 2;
    state.snapshot.slots[0].alertPersist = 1;
    state.snapshot.slots[1].name = "Highway";
    state.snapshot.slots[1].profile = "Quiet Commute";
    state.snapshot.slots[1].mode = 3;
    state.snapshot.slots[1].volume = 8;
    state.snapshot.slots[1].muteVolume = 2;
    state.snapshot.slots[1].darkMode = true;
    state.snapshot.slots[1].alertPersist = 2;
    state.snapshot.slots[1].priorityArrowOnly = true;
    state.snapshot.slots[2].name = "Comfort";
    state.snapshot.slots[2].volume = 4;
    state.snapshot.slots[2].muteVolume = 1;
    state.snapshot.slots[2].muteToZero = true;
    const WifiAutoPushApiService::Runtime runtime = makeAutoPushRuntime(state);

    WebServer slotsServer(80);
    WifiAutoPushApiService::handleApiSlots(slotsServer, runtime);
    captureResponse(captures, "autopush_routes", "GET /api/autopush/slots", slotsServer);

    WebServer activateServer(80);
    activateServer.setArg("slot", "1");
    activateServer.setArg("enable", "true");
    WifiAutoPushApiService::handleApiActivate(activateServer, runtime, nullptr, nullptr);
    captureResponse(captures, "autopush_routes", "POST /api/autopush/activate", activateServer);

    WebServer pushServer(80);
    pushServer.setArg("slot", "1");
    WifiAutoPushApiService::handleApiPushNow(pushServer, runtime, nullptr, nullptr);
    captureResponse(captures, "autopush_routes", "POST /api/autopush/push", pushServer);

    WebServer slotServer(80);
    slotServer.setArg("slot", "1");
    slotServer.setArg("profile", "Commute");
    slotServer.setArg("mode", "1");
    slotServer.setArg("name", "Highway");
    WifiAutoPushApiService::handleApiSlotSave(slotServer, runtime, nullptr, nullptr);
    captureResponse(captures, "autopush_routes", "POST /api/autopush/slot", slotServer);

    TEST_ASSERT_EQUAL_INT(1, state.activateCalls);
    TEST_ASSERT_EQUAL_INT(1, state.pushCalls);
    TEST_ASSERT_EQUAL_INT(1, state.slotSaveCalls);
}

void captureProfileRoutes(std::vector<CapturedResponse>& captures) {
    ProfileState state;
    const WifiV1ProfileApiService::Runtime runtime = makeProfileRuntime(state);

    WebServer profilesServer(80);
    WifiV1ProfileApiService::handleApiProfilesList(profilesServer, runtime);
    captureResponse(captures, "v1_profile_routes", "GET /api/v1/profiles", profilesServer);

    WebServer currentServer(80);
    WifiV1ProfileApiService::handleApiCurrentSettings(currentServer, runtime);
    captureResponse(captures, "v1_profile_routes", "GET /api/v1/current", currentServer);

    WebServer profileGetServer(80);
    profileGetServer.setArg("name", "Commute");
    WifiV1ProfileApiService::handleApiProfileGet(profileGetServer, runtime);
    captureResponse(captures, "v1_profile_routes", "GET /api/v1/profile", profileGetServer);

    WebServer pullServer(80);
    WifiV1ProfileApiService::handleApiSettingsPull(pullServer, runtime, nullptr, nullptr);
    captureResponse(captures, "v1_profile_routes", "POST /api/v1/pull", pullServer);

    WebServer profileSaveServer(80);
    profileSaveServer.setArg("plain",
                             "{\"name\":\"Commute\",\"description\":\"Daily drive\",\"settings\":{\"byte0\":1}}");
    WifiV1ProfileApiService::handleApiProfileSave(profileSaveServer, runtime, nullptr, nullptr);
    captureResponse(captures, "v1_profile_routes", "POST /api/v1/profile", profileSaveServer);

    WebServer pushServer(80);
    pushServer.setArg("plain", "{\"name\":\"Commute\"}");
    WifiV1ProfileApiService::handleApiSettingsPush(pushServer, runtime, nullptr, nullptr);
    captureResponse(captures, "v1_profile_routes", "POST /api/v1/push", pushServer);

    WebServer deleteServer(80);
    deleteServer.setArg("plain", "{\"name\":\"Commute\"}");
    WifiV1ProfileApiService::handleApiProfileDelete(deleteServer, runtime, nullptr, nullptr);
    captureResponse(captures, "v1_profile_routes", "POST /api/v1/profile/delete", deleteServer);

    TEST_ASSERT_EQUAL_INT(1, state.pullCalls);
    TEST_ASSERT_EQUAL_INT(1, state.saveCalls);
    TEST_ASSERT_EQUAL_INT(1, state.pushCalls);
    TEST_ASSERT_EQUAL_INT(1, state.deleteCalls);
}

void captureDeviceRoutes(std::vector<CapturedResponse>& captures) {
    DeviceState state;
    const WifiV1DevicesApiService::Runtime runtime = makeDeviceRuntime(state);

    WebServer listServer(80);
    WifiV1DevicesApiService::handleApiDevicesList(listServer, runtime);
    captureResponse(captures, "v1_device_routes", "GET /api/v1/devices", listServer);

    WebServer nameServer(80);
    nameServer.setArg("address", "AA:BB:CC:DD:EE:FF");
    nameServer.setArg("name", "Roadster");
    WifiV1DevicesApiService::handleApiDeviceNameSave(nameServer, runtime, nullptr, nullptr);
    captureResponse(captures, "v1_device_routes", "POST /api/v1/devices/name", nameServer);

    WebServer profileServer(80);
    profileServer.setArg("address", "AA:BB:CC:DD:EE:FF");
    profileServer.setArg("profile", "2");
    WifiV1DevicesApiService::handleApiDeviceProfileSave(profileServer, runtime, nullptr, nullptr);
    captureResponse(captures, "v1_device_routes", "POST /api/v1/devices/profile", profileServer);

    WebServer deleteServer(80);
    deleteServer.setArg("address", "AA:BB:CC:DD:EE:FF");
    WifiV1DevicesApiService::handleApiDeviceDelete(deleteServer, runtime, nullptr, nullptr);
    captureResponse(captures, "v1_device_routes", "POST /api/v1/devices/delete", deleteServer);

    TEST_ASSERT_EQUAL_INT(1, state.nameCalls);
    TEST_ASSERT_EQUAL_INT(1, state.profileCalls);
    TEST_ASSERT_EQUAL_INT(1, state.deleteCalls);
}

void captureDiagnosticsRoutes(std::vector<CapturedResponse>& captures) {
    const char* fixtureOutput = std::getenv("V1_WIFI_API_FIXTURE_OUTPUT");
    const std::filesystem::path root =
        fixtureOutput && fixtureOutput[0] != '\0'
            ? std::filesystem::path(fixtureOutput).parent_path() /
                  (std::filesystem::path(fixtureOutput).stem().string() + "-diagnostics")
            : std::filesystem::temp_directory_path() / "v1simple_wifi_fixture_remaining_emitter";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "perf", error);
    TEST_ASSERT_FALSE_MESSAGE(static_cast<bool>(error), error.message().c_str());

    fs::FS filesystem(root);
    File log = filesystem.open("/perf/perf_boot_7.csv", FILE_WRITE);
    TEST_ASSERT_TRUE(log);
    TEST_ASSERT_EQUAL_UINT32(11, log.print("header\nrow\n"));
    log.close();

    WifiDiagnosticsApiService::Runtime runtime;
    runtime.filesystem = &filesystem;
    runtime.storageReady = true;
    runtime.sdCard = true;
    runtime.maintenanceBootActive = true;

    WebServer listServer(80);
    WifiDiagnosticsApiService::handleApiList(listServer, runtime);
    captureResponse(captures, "diagnostics_routes", "GET /api/diagnostics/logs", listServer);

    WebServer downloadServer(80);
    downloadServer.setArg("path", "/perf/perf_boot_7.csv");
    WifiDiagnosticsApiService::handleApiDownload(downloadServer, runtime);
    captureResponse(captures, "diagnostics_routes", "GET /api/diagnostics/log", downloadServer, false);

    std::filesystem::remove_all(root, error);
}

void emitFixture() {
    mock_reset_heap_caps();
    std::vector<CapturedResponse> captures;
    captureCoreRoutes(captures);
    captureWifiRoutes(captures);
    captureBackupRoutes(captures);
    captureAutoPushRoutes(captures);
    captureProfileRoutes(captures);
    captureDeviceRoutes(captures);
    captureDiagnosticsRoutes(captures);
    TEST_ASSERT_EQUAL_UINT32(26, captures.size());
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_heap_caps_outstanding_allocations);

    std::ostringstream outputStream;
    outputStream << "{\"schemaVersion\":1,\"captures\":[";
    for (size_t index = 0; index < captures.size(); ++index) {
        const CapturedResponse& capture = captures[index];
        if (index > 0) {
            outputStream << ',';
        }
        outputStream << "{\"scenario\":" << jsonString(capture.scenario) << ",\"route\":" << jsonString(capture.route)
                     << ",\"status\":" << capture.status << ",\"contentType\":" << jsonString(capture.contentType)
                     << ",\"body\":" << (capture.bodyIsJson ? capture.body : jsonString(capture.body)) << '}';
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

void test_remaining_frontend_routes_emit_real_handler_responses() {
    emitFixture();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_remaining_frontend_routes_emit_real_handler_responses);
    return UNITY_END();
}
