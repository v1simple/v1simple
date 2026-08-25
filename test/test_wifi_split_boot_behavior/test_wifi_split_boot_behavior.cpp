#include <unity.h>

#include <fstream>
#include <sstream>
#include <string>

#include "../../src/modules/wifi/wifi_stop_lifecycle_policy.h"

namespace {

std::string readFile(const std::string& path) {
    std::ifstream input(path);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::string projectRoot() {
    return PROJECT_DIR;
}

std::string extractFunctionBody(const std::string& source, const std::string& signature) {
    const size_t signatureAt = source.find(signature);
    if (signatureAt == std::string::npos) {
        return {};
    }
    const size_t open = source.find('{', signatureAt);
    if (open == std::string::npos) {
        return {};
    }
    int depth = 0;
    for (size_t i = open; i < source.size(); ++i) {
        if (source[i] == '{') {
            depth++;
        } else if (source[i] == '}' && --depth == 0) {
            return source.substr(open + 1, i - open - 1);
        }
    }
    return {};
}

size_t countOccurrences(const std::string& source, const std::string& needle) {
    size_t count = 0;
    size_t at = 0;
    while ((at = source.find(needle, at)) != std::string::npos) {
        count++;
        at += needle.size();
    }
    return count;
}

} // namespace

void setUp() {}
void tearDown() {}

void test_normal_driving_boot_has_no_wifi_start_or_runtime_process_path() {
    const std::string mainSource = readFile(projectRoot() + "/src/main.cpp");
    const std::string normalSetup = extractFunctionBody(mainSource, "static void initializeStorageToReadyFlow(");
    const std::string loopBody = extractFunctionBody(mainSource, "void loop()");

    TEST_ASSERT_FALSE(normalSetup.empty());
    TEST_ASSERT_EQUAL(std::string::npos, normalSetup.find("wifiManager.process()"));
    TEST_ASSERT_EQUAL(std::string::npos, normalSetup.find("startSetupMode("));
    TEST_ASSERT_EQUAL(std::string::npos, loopBody.find("wifiManager.process()"));
    TEST_ASSERT_EQUAL(std::string::npos, loopBody.find("startSetupMode("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, loopBody.find("maintenanceRuntime.tick(millis())"));
}

void test_maintenance_boot_starts_and_processes_wifi_service() {
    const std::string source = readFile(projectRoot() + "/src/maintenance_runtime.cpp");
    const std::string maintenanceSetup = extractFunctionBody(source, "void MaintenanceRuntime::start(");
    const std::string maintenanceLoop = extractFunctionBody(source, "void MaintenanceRuntime::tick(");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, maintenanceSetup.find("configureWebApi();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, maintenanceSetup.find("wifi_.startSetupMode(false)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, maintenanceLoop.find("wifi_.process()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, maintenanceLoop.find("wifi_.isWifiServiceReachable()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, maintenanceLoop.find("wifi_.startSetupMode(false)"));
}

void test_maintenance_service_wires_routes_settings_and_runtime_status_once() {
    const std::string wiring = readFile(projectRoot() + "/src/maintenance_runtime.cpp");
    const std::string body = extractFunctionBody(wiring, "void MaintenanceRuntime::configureWebApi()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("wifiOrchestrator_.ensureCallbacksConfigured()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("wifi_.setObdDependencies"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("wifi_.setGpsRuntime"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("wifi_.appendStatusCallback"));
    TEST_ASSERT_EQUAL_UINT32(1, countOccurrences(wiring, "void MaintenanceRuntime::configureWebApi()"));
}

void test_maintenance_runtime_uses_typed_constructor_dependencies_without_provider_table() {
    const std::string header = readFile(projectRoot() + "/src/maintenance_runtime.h");
    const std::string mainSource = readFile(projectRoot() + "/src/main.cpp");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("MaintenanceRuntime(WiFiManager& wifi"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("SettingsManager& settings"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("V1ProfileManager& profiles"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("StorageManager& storage"));
    TEST_ASSERT_EQUAL(std::string::npos, header.find("struct Providers"));
    TEST_ASSERT_EQUAL(std::string::npos, mainSource.find("startSetupMode("));
}

void test_maintenance_wifi_stop_phases_advance_without_driving_loop_admission() {
    WifiStopLifecyclePolicy::PhaseInput stopHttp;
    stopHttp.idle = false;
    stopHttp.stopHttpServer = true;
    stopHttp.nowMs = 100;
    stopHttp.phaseStartMs = 100;
    stopHttp.settleMs = 50;
    TEST_ASSERT_TRUE(WifiStopLifecyclePolicy::shouldExecutePhase(stopHttp));

    WifiStopLifecyclePolicy::PhaseInput disconnectSta;
    disconnectSta.idle = false;
    disconnectSta.nowMs = 149;
    disconnectSta.phaseStartMs = 100;
    disconnectSta.settleMs = 50;
    TEST_ASSERT_FALSE(WifiStopLifecyclePolicy::shouldExecutePhase(disconnectSta));
    disconnectSta.nowMs = 150;
    TEST_ASSERT_TRUE(WifiStopLifecyclePolicy::shouldExecutePhase(disconnectSta));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_normal_driving_boot_has_no_wifi_start_or_runtime_process_path);
    RUN_TEST(test_maintenance_boot_starts_and_processes_wifi_service);
    RUN_TEST(test_maintenance_service_wires_routes_settings_and_runtime_status_once);
    RUN_TEST(test_maintenance_runtime_uses_typed_constructor_dependencies_without_provider_table);
    RUN_TEST(test_maintenance_wifi_stop_phases_advance_without_driving_loop_admission);
    return UNITY_END();
}
