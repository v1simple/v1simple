#include <cstring>

#include <unity.h>

#include "../../src/modules/alp/alp_api_service.h"
#include "../../src/modules/alp/alp_runtime_module.h"
#include "../../src/modules/gps/gps_api_service.h"
#include "../../src/modules/gps/gps_runtime_module.h"
#include "../../src/modules/obd/obd_api_service.h"
#include "../../src/modules/obd/obd_runtime_module.h"
#include "../../src/modules/wifi/wifi_autopush_api_service.h"
#include "../../src/modules/wifi/wifi_v1_profile_api_service.h"
#include "../../src/settings.h"

extern AlpRuntimeModule alpRuntimeModule;
extern GpsRuntimeModule gpsRuntimeModule;
extern ObdRuntimeModule obdRuntimeModule;

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 1000;
unsigned long mockMicros = 1000000;

namespace {

int deviceSettingsApplyCalls = 0;
int obdSettingsApplyCalls = 0;
int obdRuntimeSyncCalls = 0;
int autoPushQueueCalls = 0;
int v1ConnectedCalls = 0;
int v1PullCalls = 0;
int v1WriteCalls = 0;

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

GpsApiService::Runtime gpsRuntimePolicy(bool maintenanceBootActive) {
    GpsApiService::Runtime runtime;
    runtime.maintenanceBootActive = maintenanceBootActive;
    return runtime;
}

ObdApiService::Runtime obdRuntimePolicy(bool maintenanceBootActive) {
    ObdApiService::Runtime runtime;
    runtime.maintenanceBootActive = maintenanceBootActive;
    runtime.checkRateLimit = [](void*) { return true; };
    runtime.syncAfterConfigChange = [](void*) { ++obdRuntimeSyncCalls; };
    return runtime;
}

WifiAutoPushApiService::Runtime autoPushRuntimePolicy(bool maintenanceBootActive, bool runtimePresent) {
    WifiAutoPushApiService::Runtime runtime;
    runtime.maintenanceBootActive = maintenanceBootActive;
    if (runtimePresent) {
        runtime.queuePushNow = [](const WifiAutoPushApiService::PushNowRequest&, void*) {
            ++autoPushQueueCalls;
            return WifiAutoPushApiService::PushNowQueueResult::QUEUED;
        };
    }
    return runtime;
}

WifiV1ProfileApiService::Runtime v1RuntimePolicy(bool maintenanceBootActive, bool runtimePresent) {
    WifiV1ProfileApiService::Runtime runtime;
    runtime.maintenanceBootActive = maintenanceBootActive;
    if (runtimePresent) {
        runtime.v1Connected = [](void*) {
            ++v1ConnectedCalls;
            return true;
        };
        runtime.requestUserBytes = [](void*) {
            ++v1PullCalls;
            return true;
        };
        runtime.writeUserBytes = [](const uint8_t[6], void*) {
            ++v1WriteCalls;
            return true;
        };
    }
    return runtime;
}

int expectedLiveStatus(bool maintenanceBootActive, bool runtimePresent) {
    if (maintenanceBootActive) {
        return 409;
    }
    return runtimePresent ? 200 : 503;
}

} // namespace

SettingsManager::SettingsManager() = default;

void SettingsManager::applyDeviceSettingsUpdate(const DeviceSettingsUpdate& update, SettingsPersistMode) {
    ++deviceSettingsApplyCalls;
    if (update.hasGpsEnabled) {
        settings_.gpsEnabled = update.gpsEnabled;
    }
    if (update.hasGpsBaud) {
        settings_.gpsBaud = update.gpsBaud;
    }
    if (update.hasGpsLogUtcToPerf) {
        settings_.gpsLogUtcToPerf = update.gpsLogUtcToPerf;
    }
    if (update.hasGpsLogUtcToAlp) {
        settings_.gpsLogUtcToAlp = update.gpsLogUtcToAlp;
    }
}

bool SettingsManager::applyObdSettingsUpdate(const ObdSettingsUpdate& update, SettingsPersistMode) {
    ++obdSettingsApplyCalls;
    bool changed = false;
    if (update.hasEnabled && settings_.obdEnabled != update.enabled) {
        settings_.obdEnabled = update.enabled;
        changed = true;
    }
    if (update.hasSavedAddress && settings_.obdSavedAddress != update.savedAddress) {
        settings_.obdSavedAddress = update.savedAddress;
        changed = true;
    }
    if (update.hasSavedName && settings_.obdSavedName != update.savedName) {
        settings_.obdSavedName = update.savedName;
        changed = true;
    }
    if (update.hasSavedAddrType && settings_.obdSavedAddrType != update.savedAddrType) {
        settings_.obdSavedAddrType = update.savedAddrType;
        changed = true;
    }
    return changed;
}

void setUp() {
    alpRuntimeModule = AlpRuntimeModule();
    gpsRuntimeModule = GpsRuntimeModule();
    obdRuntimeModule = ObdRuntimeModule();
    deviceSettingsApplyCalls = 0;
    obdSettingsApplyCalls = 0;
    obdRuntimeSyncCalls = 0;
    autoPushQueueCalls = 0;
    v1ConnectedCalls = 0;
    v1PullCalls = 0;
    v1WriteCalls = 0;
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_live_status_endpoints_cover_all_mode_and_runtime_cells() {
    for (bool maintenanceBootActive : {false, true}) {
        for (bool runtimePresent : {false, true}) {
            const int expected = expectedLiveStatus(maintenanceBootActive, runtimePresent);

            WebServer alpServer(80);
            AlpApiService::handleApiStatus(alpServer, runtimePresent ? &alpRuntimeModule : nullptr, nullptr, nullptr,
                                           maintenanceBootActive);
            TEST_ASSERT_EQUAL_INT(expected, alpServer.lastStatusCode);

            WebServer gpsServer(80);
            GpsApiService::handleApiStatus(gpsServer, runtimePresent ? &gpsRuntimeModule : nullptr,
                                           gpsRuntimePolicy(maintenanceBootActive));
            TEST_ASSERT_EQUAL_INT(expected, gpsServer.lastStatusCode);

            WebServer obdServer(80);
            ObdApiService::handleApiStatus(obdServer, runtimePresent ? &obdRuntimeModule : nullptr,
                                           obdRuntimePolicy(maintenanceBootActive));
            TEST_ASSERT_EQUAL_INT(expected, obdServer.lastStatusCode);
        }
    }
}

void test_obd_scan_live_action_covers_all_mode_and_runtime_cells() {
    for (bool maintenanceBootActive : {false, true}) {
        for (bool runtimePresent : {false, true}) {
            obdRuntimeModule = ObdRuntimeModule();
            obdRuntimeModule.begin(nullptr, true, "", 0, -80);

            WebServer server(80);
            ObdApiService::handleApiScan(server, runtimePresent ? &obdRuntimeModule : nullptr,
                                         obdRuntimePolicy(maintenanceBootActive));

            TEST_ASSERT_EQUAL_INT(expectedLiveStatus(maintenanceBootActive, runtimePresent), server.lastStatusCode);
            if (maintenanceBootActive) {
                TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"maintenance_mode\""));
            }
        }
    }
}

void test_gps_config_save_is_offline_in_maintenance_only() {
    for (bool maintenanceBootActive : {false, true}) {
        for (bool runtimePresent : {false, true}) {
            SettingsManager settings;
            gpsRuntimeModule = GpsRuntimeModule();
            deviceSettingsApplyCalls = 0;

            WebServer server(80);
            server.setArg("plain", "{\"gpsEnabled\":true}");
            GpsApiService::handleApiConfigSave(server, settings, runtimePresent ? &gpsRuntimeModule : nullptr,
                                               gpsRuntimePolicy(maintenanceBootActive));

            const int expected = (!maintenanceBootActive && !runtimePresent) ? 503 : 200;
            TEST_ASSERT_EQUAL_INT(expected, server.lastStatusCode);
            TEST_ASSERT_EQUAL_INT(expected == 200 ? 1 : 0, deviceSettingsApplyCalls);
            TEST_ASSERT_EQUAL(maintenanceBootActive ? false : runtimePresent, gpsRuntimeModule.isEnabled());
        }
    }
}

void test_obd_config_save_is_offline_in_maintenance_only() {
    for (bool maintenanceBootActive : {false, true}) {
        for (bool runtimePresent : {false, true}) {
            SettingsManager settings;
            obdRuntimeModule = ObdRuntimeModule();
            obdSettingsApplyCalls = 0;
            obdRuntimeSyncCalls = 0;

            WebServer server(80);
            server.setArg("plain", "{\"enabled\":true}");
            ObdApiService::handleApiConfig(server, runtimePresent ? &obdRuntimeModule : nullptr, settings,
                                           obdRuntimePolicy(maintenanceBootActive));

            const int expected = (!maintenanceBootActive && !runtimePresent) ? 503 : 200;
            TEST_ASSERT_EQUAL_INT(expected, server.lastStatusCode);
            TEST_ASSERT_EQUAL_INT(expected == 200 ? 1 : 0, obdSettingsApplyCalls);
            TEST_ASSERT_EQUAL_INT(!maintenanceBootActive && runtimePresent ? 1 : 0, obdRuntimeSyncCalls);
        }
    }
}

void test_obd_forget_is_persistence_only_in_maintenance() {
    for (bool maintenanceBootActive : {false, true}) {
        for (bool runtimePresent : {false, true}) {
            SettingsManager settings;
            settings.mutableSettings().obdSavedAddress = "A4:C1:38:00:11:22";
            settings.mutableSettings().obdSavedName = "Adapter";
            obdRuntimeModule = ObdRuntimeModule();
            obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
            obdSettingsApplyCalls = 0;

            WebServer server(80);
            ObdApiService::handleApiForget(server, runtimePresent ? &obdRuntimeModule : nullptr, settings,
                                           obdRuntimePolicy(maintenanceBootActive));

            const int expected = (!maintenanceBootActive && !runtimePresent) ? 503 : 200;
            TEST_ASSERT_EQUAL_INT(expected, server.lastStatusCode);
            TEST_ASSERT_EQUAL_INT(expected == 200 ? 1 : 0, obdSettingsApplyCalls);
            TEST_ASSERT_EQUAL_STRING(expected == 200 ? "" : "A4:C1:38:00:11:22",
                                     settings.get().obdSavedAddress.c_str());
            if (maintenanceBootActive && runtimePresent) {
                TEST_ASSERT_TRUE(obdRuntimeModule.snapshot(mockMillis).savedAddressValid);
            }
        }
    }
}

void test_obd_device_list_offline_read_succeeds_without_runtime() {
    for (bool maintenanceBootActive : {false, true}) {
        for (bool runtimePresent : {false, true}) {
            SettingsManager settings;
            settings.mutableSettings().obdSavedAddress = "A4:C1:38:00:11:22";
            settings.mutableSettings().obdSavedName = "Adapter";

            WebServer server(80);
            ObdApiService::handleApiDevicesList(server, runtimePresent ? &obdRuntimeModule : nullptr, settings,
                                                obdRuntimePolicy(maintenanceBootActive));

            TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
            TEST_ASSERT_TRUE(responseContains(server, "\"count\":1"));
            TEST_ASSERT_TRUE(responseContains(server, "\"connected\":false"));
        }
    }
}

void test_autopush_push_live_action_covers_all_mode_and_runtime_cells() {
    for (bool maintenanceBootActive : {false, true}) {
        for (bool runtimePresent : {false, true}) {
            autoPushQueueCalls = 0;

            WebServer server(80);
            server.setArg("slot", "0");
            WifiAutoPushApiService::handleApiPushNow(
                server, autoPushRuntimePolicy(maintenanceBootActive, runtimePresent), nullptr, nullptr);

            TEST_ASSERT_EQUAL_INT(expectedLiveStatus(maintenanceBootActive, runtimePresent), server.lastStatusCode);
            TEST_ASSERT_EQUAL_INT(!maintenanceBootActive && runtimePresent ? 1 : 0, autoPushQueueCalls);
            if (!maintenanceBootActive && !runtimePresent) {
                TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"push_runtime_unavailable\""));
            }
        }
    }
}

void test_v1_pull_live_action_covers_all_mode_and_runtime_cells() {
    for (bool maintenanceBootActive : {false, true}) {
        for (bool runtimePresent : {false, true}) {
            v1ConnectedCalls = 0;
            v1PullCalls = 0;

            WebServer server(80);
            WifiV1ProfileApiService::handleApiSettingsPull(
                server, v1RuntimePolicy(maintenanceBootActive, runtimePresent), nullptr, nullptr);

            TEST_ASSERT_EQUAL_INT(expectedLiveStatus(maintenanceBootActive, runtimePresent), server.lastStatusCode);
            TEST_ASSERT_EQUAL_INT(!maintenanceBootActive && runtimePresent ? 1 : 0, v1ConnectedCalls);
            TEST_ASSERT_EQUAL_INT(!maintenanceBootActive && runtimePresent ? 1 : 0, v1PullCalls);
        }
    }
}

void test_v1_push_live_action_covers_all_mode_and_runtime_cells() {
    for (bool maintenanceBootActive : {false, true}) {
        for (bool runtimePresent : {false, true}) {
            v1ConnectedCalls = 0;
            v1WriteCalls = 0;

            WebServer server(80);
            server.setArg("plain", "{\"bytes\":[1,2,3,4,5,6],\"displayOn\":true}");
            WifiV1ProfileApiService::handleApiSettingsPush(
                server, v1RuntimePolicy(maintenanceBootActive, runtimePresent), nullptr, nullptr);

            TEST_ASSERT_EQUAL_INT(expectedLiveStatus(maintenanceBootActive, runtimePresent), server.lastStatusCode);
            TEST_ASSERT_EQUAL_INT(!maintenanceBootActive && runtimePresent ? 1 : 0, v1ConnectedCalls);
            TEST_ASSERT_EQUAL_INT(!maintenanceBootActive && runtimePresent ? 1 : 0, v1WriteCalls);
        }
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_live_status_endpoints_cover_all_mode_and_runtime_cells);
    RUN_TEST(test_obd_scan_live_action_covers_all_mode_and_runtime_cells);
    RUN_TEST(test_gps_config_save_is_offline_in_maintenance_only);
    RUN_TEST(test_obd_config_save_is_offline_in_maintenance_only);
    RUN_TEST(test_obd_forget_is_persistence_only_in_maintenance);
    RUN_TEST(test_obd_device_list_offline_read_succeeds_without_runtime);
    RUN_TEST(test_autopush_push_live_action_covers_all_mode_and_runtime_cells);
    RUN_TEST(test_v1_pull_live_action_covers_all_mode_and_runtime_cells);
    RUN_TEST(test_v1_push_live_action_covers_all_mode_and_runtime_cells);
    return UNITY_END();
}
