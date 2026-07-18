#include <unity.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

std::string extractFunctionBody(const std::string& text, const std::string& signature) {
    const size_t sigPos = text.find(signature);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, sigPos);

    const size_t braceStart = text.find('{', sigPos);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, braceStart);

    int depth = 0;
    for (size_t i = braceStart; i < text.size(); ++i) {
        if (text[i] == '{') {
            depth++;
        } else if (text[i] == '}') {
            depth--;
            if (depth == 0) {
                return text.substr(braceStart, i - braceStart + 1);
            }
        }
    }

    TEST_FAIL_MESSAGE("Failed to locate function body end");
    return {};
}

size_t countOccurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

static std::string projectRoot() {
    return std::string(PROJECT_DIR);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_main_runtime_state_tracks_maintenance_boot_mode() {
    const std::filesystem::path headerSource =
        std::filesystem::path(projectRoot() + "/include/main_runtime_state.h");
    const std::string headerText = readFile(headerSource);

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("bool maintenanceBootActive = false;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("unsigned long maintenanceBootStartedMs = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("MaintenanceBootTimeoutMs = 10UL * 60UL * 1000UL;"));
}

void test_boot_button_routes_to_maintenance_reboot_wrapper() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/main_runtime_wiring.cpp");
    const std::string text = readFile(source);
    const std::string requestBody = extractFunctionBody(text, "static void requestMaintenanceBootRestart()");
    const std::string configureBody = extractFunctionBody(text, "void configureTouchUiModule()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          requestBody.find("requestMaintenanceBoot()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          requestBody.find("ESP.restart()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          configureBody.find(".requestMaintenanceBoot = [](void* /*ctx*/) { requestMaintenanceBootRestart(); },"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      configureBody.find("wifiManager.startSetupMode(false)"));
}

void test_loop_short_circuits_normal_runtime_during_maintenance_boot() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/main.cpp");
    const std::string text = readFile(source);
    const std::string loopBody = extractFunctionBody(text, "void loop()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("if (mainRuntimeState.maintenanceBootActive)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("wifiManager.process();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("settingsManager.serviceDeferredPersist"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("settingsManager.serviceDeferredBackup"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("displayPreviewModule.update();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("displayPreviewModule.consumeEnded()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("display.showMaintenanceMode(maintenanceIp.c_str(), maintenanceStaConnected);"));
    const size_t yieldPos = loopBody.find("vTaskDelay(pdMS_TO_TICKS(1));");
    const size_t returnPos = loopBody.find("return;");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, yieldPos);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, returnPos);
    TEST_ASSERT_TRUE(yieldPos < returnPos);
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("return;"));
}

void test_setup_consumes_maintenance_request_before_runtime_init() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/main.cpp");
    const std::string text = readFile(source);
    const std::string setupBody = extractFunctionBody(text, "void setup()");
    const std::string storageBody = extractFunctionBody(text, "static void initializeStorageToReadyFlow");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          setupBody.find("const bool maintenanceBoot = readAndClearMaintenanceBootRequest();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          setupBody.find("mainRuntimeState.maintenanceBootActive = maintenanceBoot;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          storageBody.find("initializeStorageAndProfiles();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          storageBody.find("initializeMaintenanceBootFlow(setupStartMs, bootId, resetReason, logBootStage);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          storageBody.find("initializeBlePreInitAndScan"));
    TEST_ASSERT_TRUE(storageBody.find("initializeMaintenanceBootFlow") <
                     storageBody.find("initializeBlePreInitAndScan"));
}

void test_maintenance_boot_uses_dedicated_device_screen() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/main.cpp");
    const std::string text = readFile(source);
    const std::string preflightBody = extractFunctionBody(text, "static void initializePreflightDisplayAndBootUi");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          preflightBody.find("if (maintenanceBoot)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          preflightBody.find("display.showMaintenanceMode();"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      preflightBody.find("display.showDisconnected();"));
}

void test_maintenance_wifi_recovery_uses_reachability_and_retries() {
    const std::string text = readFile(projectRoot() + "/src/main.cpp");
    const std::string loopBody = extractFunctionBody(text, "void loop()");

    const size_t reachability = loopBody.find(
        "wifiRecoveryInput.wifiServiceReachable = wifiManager.isWifiServiceReachable();");
    const size_t evaluate = loopBody.find("wifiMaintenanceRecoveryModule.evaluate(wifiRecoveryInput)");
    const size_t attemptGuard = loopBody.find("if (wifiRecovery.attemptRestart)");
    const size_t restart = loopBody.find("wifiManager.startSetupMode(false)", attemptGuard);

    TEST_ASSERT_NOT_EQUAL(std::string::npos, reachability);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, evaluate);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, attemptGuard);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, restart);
    TEST_ASSERT_LESS_THAN(evaluate, reachability);
    TEST_ASSERT_LESS_THAN(attemptGuard, evaluate);
    TEST_ASSERT_LESS_THAN(restart, attemptGuard);
}

void test_maintenance_ap_bringup_failure_is_propagated() {
    const std::string header = readFile(projectRoot() + "/src/wifi_manager.h");
    const std::string source = readFile(projectRoot() + "/src/wifi_manager_lifecycle.cpp");
    const std::string setupApBody = extractFunctionBody(source, "bool WiFiManager::setupAP()");
    const std::string startBody = extractFunctionBody(source, "bool WiFiManager::startSetupMode(");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("bool setupAP();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, setupApBody.find("if (!WiFi.softAPConfig("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, setupApBody.find("if (!WiFi.softAP("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, setupApBody.find("return false;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, setupApBody.find("return true;"));
    TEST_ASSERT_EQUAL_UINT64(2, countOccurrences(startBody, "if (!setupAP())"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, startBody.find("if (!canStartSetupMode("));
}

void test_maintenance_lifecycle_suppresses_every_idle_stop() {
    const std::string source = readFile(projectRoot() + "/src/wifi_manager_lifecycle.cpp");
    const std::string processBody = extractFunctionBody(source, "void WiFiManager::process()");
    const std::string timeoutBody = extractFunctionBody(source, "void WiFiManager::checkAutoTimeout()");

    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        processBody.find("if (!maintenanceBootMode_ && apInterfaceActive && staConnectedNow"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        processBody.find("noClientInput.maintenanceBootMode = maintenanceBootMode_;"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        processBody.find("sWifiAutoTimeoutModule.evaluateNoClient(noClientInput)"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        timeoutBody.find("timeoutInput.maintenanceBootMode = maintenanceBootMode_;"));
}

void test_status_callback_publishes_maintenance_boot_fields() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/main_runtime_wiring.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "void configureWifiRuntimeModule()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("obj[\"maintenanceBoot\"] = mainRuntimeState.maintenanceBootActive;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("obj[\"maintenanceBootUptimeMs\"]"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("obj[\"maintenanceBootTimeoutMs\"] = MainRuntimePolicy::MaintenanceBootTimeoutMs;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("mainRuntimeState.maintenanceBootStartedMs"));
}

void test_loop_uses_coordinator_owned_ble_arbitration_and_obd_policy() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/main.cpp");
    const std::string text = readFile(source);
    const std::string loopBody = extractFunctionBody(text, "void loop()");

    TEST_ASSERT_EQUAL_UINT64(1, countOccurrences(loopBody, "setObdBleArbitrationRequest("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("connectionCycleCoordinatorModule.arbitrationRequest()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("obdStatus.manualScanPending"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("ObdBleArbitrationRequest::PREEMPT_PROXY_FOR_MANUAL_SCAN"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("connectionCycleCoordinatorModule.obdScanAllowed()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("connectionCycleCoordinatorModule.obdConnectAllowed()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("connectionCycleCoordinatorModule.obdRetryAllowed(now)"));
}

void test_loop_feeds_coordinator_proxy_policy_into_ble() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/main.cpp");
    const std::string text = readFile(source);
    const std::string loopBody = extractFunctionBody(text, "void loop()");

    TEST_ASSERT_EQUAL_UINT64(1, countOccurrences(loopBody, "setConnectionCycleProxyPolicy("));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        loopBody.find("bleClient.setConnectionCycleProxyPolicy("));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        loopBody.find("connectionCycleCoordinatorModule.proxyAdvertisingAllowed()"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        loopBody.find("connectionCycleCoordinatorModule.proxyKeepConnectionAllowed()"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        loopBody.find("bleClient.setConnectionCycleState("));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        loopBody.find("bleClient.hasProxyClientConnectedThisBoot()"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        loopBody.find("currentSettings.wifiOpenTimeoutMs"));
}

void test_loop_refreshes_coordinator_proxy_policy_after_ingest_phase() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/main.cpp");
    const std::string text = readFile(source);
    const std::string loopBody = extractFunctionBody(text, "void loop()");

    const size_t ingestPos = loopBody.find("const LoopIngestPhaseValues loopIngestValues = processLoopIngestPhase(");
    const size_t coordinatorUpdatePos =
        loopBody.find("connectionCycleCoordinatorModule.update(cycleContext)");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, ingestPos);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, coordinatorUpdatePos);
    TEST_ASSERT_LESS_THAN(coordinatorUpdatePos, ingestPos);
}

void test_loop_feeds_coordinator_wifi_policy_into_wifi_phase() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/main.cpp");
    const std::string text = readFile(source);
    const std::string loopBody = extractFunctionBody(text, "void loop()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("connectionCycleCoordinatorModule.wifiAutoStartAllowed()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("mainRuntimeState.wifiManualStartIntentLatched,"));
}

void test_configure_system_loop_modules_wires_coordinator_control_surfaces() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/main_runtime_wiring.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "void configureSystemLoopModules()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("cycleProviders.stopObdScan ="));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("cycleProviders.cancelObdConnect ="));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("cycleProviders.stopProxyAdvertising ="));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("cycleProviders.disconnectProxyPhone ="));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("cycleProviders.isProxyFullyStopped ="));
}

void test_obd_runtime_config_change_skips_proxy_in_maintenance() {
    // Regression: makeObdRuntime().syncAfterConfigChange must short-circuit in
    // maintenance boot, mirroring the guards in applySettingsUpdate and
    // syncAfterRestore. Without this, an OBD config save in maintenance mode
    // can drive bleClient.setProxyRuntimeEnabled(true) → initProxyServer() →
    // NimBLEDevice::createServer() against an uninitialized NimBLE stack.
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/wifi_runtimes.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(
        text, "ObdApiService::Runtime WiFiManager::makeObdRuntime()");

    const size_t syncStart = body.find("r.syncAfterConfigChange = [](void* ctx) {");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, syncStart);
    const size_t proxyCall = body.find("bleClient.setProxyRuntimeEnabled", syncStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, proxyCall);
    const size_t guardPos = body.find("isMaintenanceBootMode()", syncStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, guardPos);
    // Guard must precede the BLE/runtime calls so it short-circuits before
    // touching uninitialized peripherals.
    TEST_ASSERT_LESS_THAN(proxyCall, guardPos);
}

void test_device_settings_save_persists_immediately_in_maintenance() {
    // Regression: dashboard "Proxy / App" mode is saved via
    // /api/device/settings while the device is in maintenance Wi-Fi boot.
    // Maintenance used to request a deferred persist without servicing that
    // deferred queue, so a reboot could lose proxy_ble before NVS was written.
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/wifi_runtimes.cpp");
    const std::string text = readFile(source);
    const std::string body =
        extractFunctionBody(text, "WifiSettingsApiService::Runtime WiFiManager::makeSettingsRuntime()");

    const size_t lambdaStart =
        body.find("r.applySettingsUpdate = [](const DeviceSettingsUpdate& update, void* ctx)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, lambdaStart);
    const size_t maintenanceCheck = body.find("isMaintenanceBootMode()", lambdaStart);
    const size_t immediateMode = body.find("SettingsPersistMode::Immediate", lambdaStart);
    const size_t nvsDeferredBackupMode =
        body.find("SettingsPersistMode::ImmediateNvsDeferredBackup", lambdaStart);
    const size_t applyUpdate = body.find("settingsManager.applyDeviceSettingsUpdate", lambdaStart);
    const size_t bleProxyCall = body.find("bleClient.setProxyRuntimeEnabled", lambdaStart);

    TEST_ASSERT_NOT_EQUAL(std::string::npos, maintenanceCheck);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, immediateMode);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, nvsDeferredBackupMode);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, applyUpdate);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, bleProxyCall);

    // The maintenance decision must feed the persist mode before any live BLE
    // runtime sync; BLE is intentionally not initialized in maintenance boot.
    TEST_ASSERT_LESS_THAN(applyUpdate, maintenanceCheck);
    TEST_ASSERT_LESS_THAN(bleProxyCall, applyUpdate);
}

void test_web_ui_runtime_saves_persist_nvs_before_deferred_sd_backup() {
    // Explicit Web UI save endpoints should not acknowledge a save while only
    // an idle-time NVS write is pending.  They persist NVS synchronously, then
    // defer only the SD mirror so reboot/power loss after success keeps the
    // user's new settings.
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/wifi_runtimes.cpp");
    const std::string text = readFile(source);

    const char* signatures[] = {
        "WifiAutoPushApiService::Runtime WiFiManager::makeAutoPushRuntime()",
        "WifiDisplayColorsApiService::Runtime WiFiManager::makeDisplayColorsRuntime()",
        "WifiQuietApiService::Runtime WiFiManager::makeQuietRuntime()",
        "WifiSettingsApiService::Runtime WiFiManager::makeSettingsRuntime()",
    };

    for (const char* signature : signatures) {
        const std::string body = extractFunctionBody(text, signature);
        TEST_ASSERT_NOT_EQUAL(
            std::string::npos,
            body.find("SettingsPersistMode::ImmediateNvsDeferredBackup"));
        TEST_ASSERT_EQUAL(std::string::npos,
                          body.find("SettingsPersistMode::Deferred"));
    }
}

void test_obd_config_save_persists_immediately_in_maintenance() {
    // Dashboard mode save also writes /api/obd/config. In maintenance boot it
    // must be an immediate NVS save for the same reason as /api/device/settings.
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/modules/obd/obd_api_service.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(
        text, "void handleApiConfig(WebServer& server,");

    const size_t applyUpdate = body.find("settings.applyObdSettingsUpdate(");
    const size_t maintenanceCheck = body.find("runtime.maintenanceBootActive", applyUpdate);
    const size_t immediateMode = body.find("SettingsPersistMode::Immediate", applyUpdate);
    const size_t nvsDeferredBackupMode =
        body.find("SettingsPersistMode::ImmediateNvsDeferredBackup", applyUpdate);
    const size_t syncCall = body.find("runtime.syncAfterConfigChange", applyUpdate);

    TEST_ASSERT_NOT_EQUAL(std::string::npos, applyUpdate);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, maintenanceCheck);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, immediateMode);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, nvsDeferredBackupMode);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, syncCall);
    TEST_ASSERT_LESS_THAN(maintenanceCheck, applyUpdate);
    TEST_ASSERT_LESS_THAN(syncCall, applyUpdate);
}

void test_gps_config_save_skips_live_runtime_in_maintenance() {
    // Regression: handleApiConfigSave must persist the setting via
    // applyDeviceSettingsUpdate() but NOT bring the GPS UART up via
    // gpsRuntime.setEnabled()/setBaud() when maintenanceBootActive is true.
    // Maintenance boot intentionally skips GPS init; live runtime resumes on
    // next normal boot.
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/modules/gps/gps_api_service.cpp");
    const std::string text = readFile(source);
    const std::string body =
        extractFunctionBody(text, "void handleApiConfigSave(WebServer& server,");

    const size_t applyUpdate = body.find("settings.applyDeviceSettingsUpdate(");
    const size_t guardPos = body.find("runtime.maintenanceBootActive");
    const size_t immediateMode = body.find("SettingsPersistMode::Immediate", applyUpdate);
    const size_t nvsDeferredBackupMode =
        body.find("SettingsPersistMode::ImmediateNvsDeferredBackup", applyUpdate);
    const size_t setEnabledCall = body.find("gpsRuntime.setEnabled(s.gpsEnabled)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, applyUpdate);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, guardPos);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, immediateMode);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, nvsDeferredBackupMode);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, setEnabledCall);
    // Settings must persist before the guard, and the guard must precede the
    // live-runtime application so maintenance mode keeps the UART quiesced.
    TEST_ASSERT_LESS_THAN(guardPos, applyUpdate);
    TEST_ASSERT_LESS_THAN(setEnabledCall, guardPos);
}

void test_gps_config_post_route_propagates_maintenance_boot_state() {
    // The service-level guard is only effective when the route composition
    // passes the boot mode through. Regression: POST /api/gps/config once
    // constructed a default Runtime (maintenance=false) and started Serial1
    // from maintenance mode despite the persist-only service contract.
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/wifi_routes.cpp");
    const std::string text = readFile(source);

    const size_t routeStart = text.find("server_.on(\"/api/gps/config\", HTTP_POST");
    const size_t nextRoute = text.find("server_.on(\"/api/gps/status\", HTTP_GET", routeStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, routeStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, nextRoute);

    const std::string routeBody = text.substr(routeStart, nextRoute - routeStart);
    const size_t propagation = routeBody.find(
        "r.maintenanceBootActive = mainRuntimeState.maintenanceBootActive;");
    const size_t delegate = routeBody.find("GpsApiService::handleApiConfigSave(");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, propagation);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, delegate);
    TEST_ASSERT_LESS_THAN(delegate, propagation);
}

void test_autopush_runtime_propagates_maintenance_boot_state() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/wifi_runtimes.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(
        text, "WifiAutoPushApiService::Runtime WiFiManager::makeAutoPushRuntime()");

    const size_t propagation = body.find(
        "runtime.maintenanceBootActive = mainRuntimeState.maintenanceBootActive;");
    const size_t returnRuntime = body.find("return runtime;");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, propagation);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, returnRuntime);
    TEST_ASSERT_LESS_THAN(returnRuntime, propagation);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_main_runtime_state_tracks_maintenance_boot_mode);
    RUN_TEST(test_boot_button_routes_to_maintenance_reboot_wrapper);
    RUN_TEST(test_loop_short_circuits_normal_runtime_during_maintenance_boot);
    RUN_TEST(test_setup_consumes_maintenance_request_before_runtime_init);
    RUN_TEST(test_maintenance_boot_uses_dedicated_device_screen);
    RUN_TEST(test_maintenance_wifi_recovery_uses_reachability_and_retries);
    RUN_TEST(test_maintenance_ap_bringup_failure_is_propagated);
    RUN_TEST(test_maintenance_lifecycle_suppresses_every_idle_stop);
    RUN_TEST(test_status_callback_publishes_maintenance_boot_fields);
    RUN_TEST(test_loop_uses_coordinator_owned_ble_arbitration_and_obd_policy);
    RUN_TEST(test_loop_feeds_coordinator_proxy_policy_into_ble);
    RUN_TEST(test_loop_refreshes_coordinator_proxy_policy_after_ingest_phase);
    RUN_TEST(test_loop_feeds_coordinator_wifi_policy_into_wifi_phase);
    RUN_TEST(test_configure_system_loop_modules_wires_coordinator_control_surfaces);
    RUN_TEST(test_obd_runtime_config_change_skips_proxy_in_maintenance);
    RUN_TEST(test_device_settings_save_persists_immediately_in_maintenance);
    RUN_TEST(test_web_ui_runtime_saves_persist_nvs_before_deferred_sd_backup);
    RUN_TEST(test_obd_config_save_persists_immediately_in_maintenance);
    RUN_TEST(test_gps_config_save_skips_live_runtime_in_maintenance);
    RUN_TEST(test_gps_config_post_route_propagates_maintenance_boot_state);
    RUN_TEST(test_autopush_runtime_propagates_maintenance_boot_state);
    return UNITY_END();
}
