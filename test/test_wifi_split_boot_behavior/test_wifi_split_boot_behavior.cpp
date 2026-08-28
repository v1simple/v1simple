#include <unity.h>

#include <fstream>
#include <sstream>
#include <string>

#include "../../src/modules/wifi/wifi_stop_lifecycle_policy.h"
#include "../../src/runtime_coordinator.h"

namespace {

struct FakeRecoveryResult {
    bool attemptRestart = false;
    uint32_t attemptNumber = 0;
};

struct FakeDriveRuntime {
    int startCalls = 0;
    int tickCalls = 0;

    void start(uint32_t, uint32_t, int) { ++startCalls; }
    void tick() { ++tickCalls; }
};

struct FakeMaintenanceRuntime {
    bool activeValue = false;
    bool presentationSuppressed = false;
    bool wifiActive = false;
    FakeRecoveryResult recovery;
    int startCalls = 0;
    int tickCalls = 0;
    int wifiStartCalls = 0;
    int wifiProcessCalls = 0;
    int recoveryCalls = 0;
    int wifiRestartCalls = 0;
    int wifiStopCalls = 0;
    uint32_t restartAttemptNumber = 0;

    void start(uint32_t, int) {
        ++startCalls;
        activeValue = true;
        MaintenanceWifiCoordinator::start(*this);
    }

    bool active() const { return activeValue; }

    void tick(uint32_t nowMs) {
        ++tickCalls;
        MaintenanceWifiCoordinator::service(*this, nowMs, presentationSuppressed);
    }

    void stop() {
        MaintenanceWifiCoordinator::stop(*this, "maintenance_stop");
        activeValue = false;
    }

    bool startMaintenanceWifi() {
        ++wifiStartCalls;
        wifiActive = true;
        return true;
    }

    void processMaintenanceWifi() { ++wifiProcessCalls; }

    FakeRecoveryResult evaluateMaintenanceWifiRecovery(uint32_t) {
        ++recoveryCalls;
        return recovery;
    }

    void restartMaintenanceWifi(uint32_t attemptNumber) {
        ++wifiRestartCalls;
        restartAttemptNumber = attemptNumber;
        wifiActive = true;
    }

    bool maintenanceWifiActive() const { return wifiActive; }

    void stopMaintenanceWifi(const char*) {
        ++wifiStopCalls;
        wifiActive = false;
    }
};

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

void test_normal_boot_executes_drive_only_and_never_touches_wifi_runtime() {
    FakeDriveRuntime drive;
    FakeMaintenanceRuntime maintenance;
    int clockReads = 0;

    MainRuntimeCoordinator::start(false, 10, 11, 1, drive, maintenance);
    MainRuntimeCoordinator::tick(drive, maintenance, [&clockReads]() {
        ++clockReads;
        return 20U;
    });

    TEST_ASSERT_EQUAL(1, drive.startCalls);
    TEST_ASSERT_EQUAL(1, drive.tickCalls);
    TEST_ASSERT_EQUAL(0, maintenance.startCalls);
    TEST_ASSERT_EQUAL(0, maintenance.tickCalls);
    TEST_ASSERT_EQUAL(0, maintenance.wifiStartCalls);
    TEST_ASSERT_EQUAL(0, maintenance.wifiProcessCalls);
    TEST_ASSERT_EQUAL(0, clockReads);
}

void test_maintenance_boot_starts_services_without_wifi_master_setting_dependency() {
    FakeDriveRuntime drive;
    FakeMaintenanceRuntime maintenance;

    const std::string coordinator = readFile(projectRoot() + "/src/runtime_coordinator.h");
    const std::string runtime = readFile(projectRoot() + "/src/maintenance_runtime.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, coordinator.find("enableWifi"));
    TEST_ASSERT_EQUAL(std::string::npos, runtime.find("enableWifi"));

    MainRuntimeCoordinator::start(true, 10, 11, 1, drive, maintenance);
    MainRuntimeCoordinator::tick(drive, maintenance, []() { return 25U; });
    maintenance.stop();

    TEST_ASSERT_EQUAL(0, drive.startCalls);
    TEST_ASSERT_EQUAL(0, drive.tickCalls);
    TEST_ASSERT_EQUAL(1, maintenance.startCalls);
    TEST_ASSERT_EQUAL(1, maintenance.tickCalls);
    TEST_ASSERT_EQUAL(1, maintenance.wifiStartCalls);
    TEST_ASSERT_EQUAL(1, maintenance.wifiProcessCalls);
    TEST_ASSERT_EQUAL(1, maintenance.recoveryCalls);
    TEST_ASSERT_EQUAL(1, maintenance.wifiStopCalls);
    TEST_ASSERT_FALSE(maintenance.wifiActive);
    TEST_ASSERT_FALSE(maintenance.active());
}

void test_maintenance_wifi_recovery_restarts_an_unreachable_service() {
    FakeMaintenanceRuntime maintenance;
    maintenance.activeValue = true;
    maintenance.recovery.attemptRestart = true;
    maintenance.recovery.attemptNumber = 3;

    MaintenanceWifiCoordinator::service(maintenance, 500, false);

    TEST_ASSERT_EQUAL(1, maintenance.wifiProcessCalls);
    TEST_ASSERT_EQUAL(1, maintenance.recoveryCalls);
    TEST_ASSERT_EQUAL(1, maintenance.wifiRestartCalls);
    TEST_ASSERT_EQUAL_UINT32(3, maintenance.restartAttemptNumber);
}

void test_power_presentation_suppresses_maintenance_wifi_service() {
    FakeMaintenanceRuntime maintenance;
    maintenance.activeValue = true;

    MaintenanceWifiCoordinator::service(maintenance, 500, true);

    TEST_ASSERT_EQUAL(0, maintenance.wifiProcessCalls);
    TEST_ASSERT_EQUAL(0, maintenance.recoveryCalls);
    TEST_ASSERT_EQUAL(0, maintenance.wifiRestartCalls);
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

    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("MaintenanceRuntime(WiFiManager& wifi"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("SettingsManager& settings"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("V1ProfileManager& profiles"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("StorageManager& storage"));
    TEST_ASSERT_EQUAL(std::string::npos, header.find("struct Providers"));
}

void test_maintenance_runtime_start_reaches_saved_network_auto_join() {
    const std::string runtime = readFile(projectRoot() + "/src/maintenance_runtime.cpp");
    const std::string lifecycle = readFile(projectRoot() + "/src/wifi_manager_lifecycle.cpp");
    const std::string runtimeStart = extractFunctionBody(runtime, "bool MaintenanceRuntime::startMaintenanceWifi(");
    const std::string setupStart = extractFunctionBody(lifecycle, "bool WiFiManager::startSetupMode(");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, runtimeStart.find("wifi_.startSetupMode(false)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, setupStart.find("selectSavedNetworkStart"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, setupStart.find("beginMaintenanceAutoConnectScan(false)"));
}

void test_saved_network_test_persists_enable_before_replacing_runtime_activity() {
    const std::string client = readFile(projectRoot() + "/src/wifi_client.cpp");
    const std::string body = extractFunctionBody(client, "bool WiFiManager::testSavedNetwork(");
    const size_t persistAt = body.find("setWifiClientEnabled(true)");
    const size_t cancelAt = body.find("cancelMaintenanceAutoConnect(\"slot_test\")");
    const size_t queueAt = body.find("connectToNetwork(");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, persistAt);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, cancelAt);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, queueAt);
    TEST_ASSERT_TRUE(persistAt < cancelAt);
    TEST_ASSERT_TRUE(cancelAt < queueAt);
}

void test_explicit_disconnect_arms_session_suppression_and_enable_test_clear_it() {
    const std::string client = readFile(projectRoot() + "/src/wifi_client.cpp");
    const std::string disconnect = extractFunctionBody(client, "void WiFiManager::disconnectFromNetwork(");
    const std::string test = extractFunctionBody(client, "bool WiFiManager::testSavedNetwork(");
    const std::string enableScan = extractFunctionBody(client, "bool WiFiManager::beginMaintenanceAutoConnectScan(");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, disconnect.find("maintenanceManualDisconnect_ = true"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, disconnect.find("maintenanceAutoConnectRetryAtMs_ = 0"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, test.find("maintenanceManualDisconnect_ = false"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, enableScan.find("maintenanceManualDisconnect_ = false"));
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
    RUN_TEST(test_normal_boot_executes_drive_only_and_never_touches_wifi_runtime);
    RUN_TEST(test_maintenance_boot_starts_services_without_wifi_master_setting_dependency);
    RUN_TEST(test_maintenance_wifi_recovery_restarts_an_unreachable_service);
    RUN_TEST(test_power_presentation_suppresses_maintenance_wifi_service);
    RUN_TEST(test_maintenance_service_wires_routes_settings_and_runtime_status_once);
    RUN_TEST(test_maintenance_runtime_uses_typed_constructor_dependencies_without_provider_table);
    RUN_TEST(test_maintenance_runtime_start_reaches_saved_network_auto_join);
    RUN_TEST(test_saved_network_test_persists_enable_before_replacing_runtime_activity);
    RUN_TEST(test_explicit_disconnect_arms_session_suppression_and_enable_test_clear_it);
    RUN_TEST(test_maintenance_wifi_stop_phases_advance_without_driving_loop_admission);
    return UNITY_END();
}
