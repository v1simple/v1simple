#include <unity.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../../include/ble_log_rate_limit.h"

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

static std::string projectRoot() {
    return std::string(PROJECT_DIR);
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

}  // namespace

void setUp() {}
void tearDown() {}

void test_ble_connection_log_rate_limit_allows_first_then_bounds_burst() {
    BleLogRateLimitState state{};
    uint32_t allowed = 0;
    for (uint32_t nowMs = 1000; nowMs < 10000; nowMs += 1000) {
        if (shouldLogBleConnectionEvent(state, nowMs)) {
            allowed++;
        }
    }

    TEST_ASSERT_EQUAL_UINT32(1, allowed);
    TEST_ASSERT_FALSE(shouldLogBleConnectionEvent(state, 10999));
    TEST_ASSERT_TRUE(shouldLogBleConnectionEvent(state, 11000));
}

void test_async_connect_does_not_delete_bond() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "bool V1BLEClient::startAsyncConnect()");

    TEST_ASSERT_EQUAL(std::string::npos, body.find("NimBLEDevice::deleteBond("));
}

void test_disconnect_callback_still_defers_bond_heal() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::string text = readFile(source);
    const std::string body =
        extractFunctionBody(text, "void V1BLEClient::ClientCallbacks::onDisconnect");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("pendingDeleteBondAddr_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("pendingDeleteBond_ = true"));
}

void test_disconnect_callback_no_longer_stops_proxy_advertising_inline() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::string text = readFile(source);
    const std::string body =
        extractFunctionBody(text, "void V1BLEClient::ClientCallbacks::onDisconnect");

    TEST_ASSERT_EQUAL(std::string::npos, body.find("NimBLEDevice::stopAdvertising("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("enqueueProxyCallbackEvent"));
}

void test_v1_connection_event_timestamp_is_written_on_connect_and_disconnect() {
    const std::filesystem::path headerSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.h");
    const std::filesystem::path connectionSource =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::string headerText = readFile(headerSource);
    const std::string connectionText = readFile(connectionSource);
    const std::string subscribeBody = extractFunctionBody(
        connectionText, "void V1BLEClient::processSubscribing()");
    const std::string disconnectBody = extractFunctionBody(
        connectionText, "void V1BLEClient::ClientCallbacks::onDisconnect");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("std::atomic<uint32_t> lastV1ConnectionEventMs_{0};"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          subscribeBody.find("lastV1ConnectionEventMs_.store(connectedNowMs"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        disconnectBody.find("lastV1ConnectionEventMs_.store(static_cast<uint32_t>(millis())"));
}

void test_verify_push_edge_state_is_tracked_in_header_and_commands() {
    const std::filesystem::path headerSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.h");
    const std::filesystem::path commandsSource =
        std::filesystem::path(projectRoot() + "/src/ble_commands.cpp");
    const std::string headerText = readFile(headerSource);
    const std::string commandsText = readFile(commandsSource);
    const std::string startVerifyBody = extractFunctionBody(
        commandsText, "void V1BLEClient::startUserBytesVerification");
    const std::string onUserBytesBody = extractFunctionBody(
        commandsText, "void V1BLEClient::onUserBytesReceived");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("bool consumeVerifyPushMatchEdge()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("std::atomic<bool> verifyPushMatchEdgePending_{false};"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          startVerifyBody.find("verifyPushMatchEdgePending_.store(false"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          onUserBytesBody.find("verifyPushMatchEdgePending_.store(verifyMatch_"));
}

void test_proxy_control_wrappers_are_exposed_and_delegate_to_existing_proxy_logic() {
    const std::filesystem::path headerSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.h");
    const std::filesystem::path proxySource =
        std::filesystem::path(projectRoot() + "/src/ble_proxy.cpp");
    const std::filesystem::path clientSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.cpp");
    const std::string headerText = readFile(headerSource);
    const std::string proxyText = readFile(proxySource);
    const std::string clientText = readFile(clientSource);
    const std::string stopBody =
        extractFunctionBody(proxyText, "void V1BLEClient::stopProxyAdvertising()");
    const std::string disconnectBody =
        extractFunctionBody(proxyText, "void V1BLEClient::disconnectProxyPhones()");
    const std::string fullyStoppedBody =
        extractFunctionBody(proxyText, "bool V1BLEClient::isProxyFullyStopped() const");
    const std::string policyBody =
        extractFunctionBody(clientText, "void V1BLEClient::setConnectionCycleProxyPolicy");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("void stopProxyAdvertising();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("void disconnectProxyPhones();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("bool isProxyFullyStopped() const;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("bool hasProxyClientConnectedThisBoot() const"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("void setConnectionCycleProxyPolicy(bool advertisingAllowed, bool keepConnectionAllowed);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("void setConnectionCycleState(uint8_t stateCode, uint32_t timeInStateMs);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          stopBody.find("stopProxyAdvertisingFromMainLoop("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          disconnectBody.find("proxySuppressedForObdHold_ = true"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          disconnectBody.find("for (uint16_t h : pServer_->getPeerDevices())"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          disconnectBody.find("pServer_->disconnect(h);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          fullyStoppedBody.find("proxyAdvertisingStartMs_ == 0"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      fullyStoppedBody.find("proxyAdvertisingRetryAtMs_ == 0"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          fullyStoppedBody.find("pServer_->getConnectedCount() > 0"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          policyBody.find("proxyAdvertisingAllowed_ = advertisingAllowed"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          policyBody.find("proxyKeepConnectionAllowed_ = keepConnectionAllowed"));
}

void test_proxy_app_mode_runtime_toggle_and_local_write_contract() {
    const std::filesystem::path headerSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.h");
    const std::filesystem::path proxySource =
        std::filesystem::path(projectRoot() + "/src/ble_proxy.cpp");
    const std::filesystem::path commandsSource =
        std::filesystem::path(projectRoot() + "/src/ble_commands.cpp");
    const std::string headerText = readFile(headerSource);
    const std::string proxyText = readFile(proxySource);
    const std::string commandsText = readFile(commandsSource);
    const std::string runtimeToggleBody =
        extractFunctionBody(proxyText, "bool V1BLEClient::setProxyRuntimeEnabled");
    const std::string suppressBody =
        extractFunctionBody(commandsText, "bool V1BLEClient::localV1WriteSuppressedByProxy");
    const std::string phoneCommandBody =
        extractFunctionBody(proxyText, "int V1BLEClient::processPhoneCommandQueue()");
    const std::string sendCommandBody =
        extractFunctionBody(commandsText, "SendResult V1BLEClient::sendCommandWithResult");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("bool setProxyRuntimeEnabled(bool enabled"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("bool localV1WriteSuppressedByProxy(const char* operation) const"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, runtimeToggleBody.find("initProxyServer("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, runtimeToggleBody.find("allocateProxyQueues()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, runtimeToggleBody.find("disconnectProxyPhones();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, runtimeToggleBody.find("releaseProxyQueues();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          suppressBody.find("proxyClientConnected_.load(std::memory_order_relaxed)"));

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          extractFunctionBody(commandsText, "bool V1BLEClient::setDisplayOn").find("localV1WriteSuppressedByProxy"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          extractFunctionBody(commandsText, "bool V1BLEClient::setMute").find("localV1WriteSuppressedByProxy"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          extractFunctionBody(commandsText, "bool V1BLEClient::setMode").find("localV1WriteSuppressedByProxy"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          extractFunctionBody(commandsText, "bool V1BLEClient::setVolume").find("localV1WriteSuppressedByProxy"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          extractFunctionBody(commandsText, "bool V1BLEClient::writeUserBytes").find("localV1WriteSuppressedByProxy"));

    // Proxy-app-originated raw V1 writes still flow through the phone queue; the
    // suppression guard is only for local V1 Simple features.
    TEST_ASSERT_EQUAL(std::string::npos, phoneCommandBody.find("localV1WriteSuppressedByProxy"));
    TEST_ASSERT_EQUAL(std::string::npos, sendCommandBody.find("localV1WriteSuppressedByProxy"));
}

void test_proxy_server_is_preinitialized_before_runtime_toggle() {
    // Regression: the serial bench/debug protocol can force proxy mode after
    // boot. NimBLE 2.5 asserts if the proxy server is first created after the
    // scan/connect path is active, so initBLE must pre-create the server and
    // leave runtime toggles to queue/advertising policy only.
    const std::filesystem::path headerSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.h");
    const std::filesystem::path clientSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.cpp");
    const std::filesystem::path proxySource =
        std::filesystem::path(projectRoot() + "/src/ble_proxy.cpp");
    const std::string headerText = readFile(headerSource);
    const std::string clientText = readFile(clientSource);
    const std::string proxyText = readFile(proxySource);
    const std::string initBody = extractFunctionBody(clientText, "bool V1BLEClient::initBLE");
    const std::string runtimeToggleBody =
        extractFunctionBody(proxyText, "bool V1BLEClient::setProxyRuntimeEnabled");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("bool proxyServerInitAttempted_;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, initBody.find("proxyServerInitAttempted_ = true;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          initBody.find("proxyServerInitialized_ = initProxyServer(proxyName_.c_str())"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, initBody.find("releaseProxyQueues();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          runtimeToggleBody.find("if (proxyServerInitAttempted_)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          runtimeToggleBody.find("runtime enable requires reboot"));
}

void test_explicit_proxy_mode_gates_proxy_policy_and_drops_obd() {
    const std::filesystem::path mainSource =
        std::filesystem::path(projectRoot() + "/src/main.cpp");
    const std::filesystem::path wifiRuntimeSource =
        std::filesystem::path(projectRoot() + "/src/wifi_runtimes.cpp");
    const std::filesystem::path obdRuntimeSource =
        std::filesystem::path(projectRoot() + "/src/modules/obd/obd_runtime_module.cpp");
    const std::string mainText = readFile(mainSource);
    const std::string wifiRuntimeText = readFile(wifiRuntimeSource);
    const std::string obdRuntimeText = readFile(obdRuntimeSource);
    const std::string loopBody = extractFunctionBody(mainText, "void loop()");
    const std::string obdUpdateBody =
        extractFunctionBody(obdRuntimeText, "void ObdRuntimeModule::update");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("currentSettings.proxyBLE && bleClient.isProxyEnabled()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("proxyModeEnabled && connectionCycleCoordinatorModule.proxyAdvertisingAllowed()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("proxyModeEnabled && connectionCycleCoordinatorModule.proxyKeepConnectionAllowed()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, loopBody.find("if (bleClient.isProxyClientConnected())"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, loopBody.find("obdRuntimeModule.stopActiveScan();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, loopBody.find("obdRuntimeModule.cancelPendingConnect();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          wifiRuntimeText.find("bleClient.setProxyRuntimeEnabled(settings.proxyBLE, settings.proxyName.c_str())"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, obdUpdateBody.find("if (proxyClientConnected)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, obdUpdateBody.find("transitionTo(ObdConnectionState::IDLE"));
}

void test_connection_cycle_state_setter_updates_diagnostic_cache() {
    const std::filesystem::path clientSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.cpp");
    const std::string clientText = readFile(clientSource);
    const std::string body =
        extractFunctionBody(clientText, "void V1BLEClient::setConnectionCycleState");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("connectionCycleStateCode_ = stateCode"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("connectionCycleTimeInStateMs_ = timeInStateMs"));
}

void test_app_disconnect_no_longer_reschedules_proxy_inline() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_proxy.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(
        text, "void V1BLEClient::handleProxyCallbackEvent(const ProxyCallbackEvent& event)");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("clearProxyAdvertisingSchedule();"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      body.find("proxyAdvertisingStartMs_ = static_cast<uint32_t>(millis())"));
}

void test_manual_obd_preempt_disconnects_proxy_from_main_loop() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "void V1BLEClient::process()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("PREEMPT_PROXY_FOR_MANUAL_SCAN"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("stopProxyAdvertisingFromMainLoop("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("pServer_->disconnect("));
}

void test_connected_followup_no_longer_schedules_proxy_advertising() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_connected_followup.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "void V1BLEClient::processConnectedFollowup()");

    TEST_ASSERT_EQUAL(std::string::npos,
                      body.find("ConnectedFollowupStep::SCHEDULE_PROXY_ADVERTISING"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      body.find("proxyAdvertisingStartMs_ = static_cast<uint32_t>(millis()) + PROXY_STABILIZE_MS"));
}

void test_runtime_uses_coordinator_proxy_policy_instead_of_proxy_windows() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "void V1BLEClient::process()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("const bool proxyAdvertisingAllowed ="));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("proxyAdvertisingAllowed_ &&"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("const bool proxyKeepConnectionAllowed ="));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("proxyKeepConnectionAllowed_ && !preemptProxyForManualScan"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      body.find("proxyAdvertisingRetryAtMs_ = nowMs + PROXY_ADVERTISING_RETRY_MS"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      body.find("Proxy idle window elapsed; pausing advertising"));
}

void test_runtime_proxy_guard_keeps_non_coordinator_safety_clauses() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "void V1BLEClient::process()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("!wifiPriorityMode_"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      body.find("proxyNoClientTimeoutLatched_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("!suppressPassiveProxy"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("refreshProxyAdvertisingCadence("));
}

void test_runtime_process_consolidates_proxy_and_state_machine_millis_sample() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "void V1BLEClient::process()");

    TEST_ASSERT_EQUAL_UINT64(2, countOccurrences(body, "millis()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("const uint32_t now = static_cast<uint32_t>(millis());"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("proxyAdvertisingStartMs_ = now + PROXY_STABILIZE_MS;"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      body.find("const uint32_t nowMs = static_cast<uint32_t>(millis());"));
}

void test_noncritical_ble_connection_logs_are_rate_limited_or_callback_disabled() {
    const std::filesystem::path connectionSource =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::filesystem::path runtimeSource =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::filesystem::path followupSource =
        std::filesystem::path(projectRoot() + "/src/ble_connected_followup.cpp");
    const std::string connectionText = readFile(connectionSource);
    const std::string runtimeText = readFile(runtimeSource);
    const std::string followupText = readFile(followupSource);
    const std::string phyUpdateBody =
        extractFunctionBody(connectionText, "void V1BLEClient::ClientCallbacks::onPhyUpdate");
    const std::string startAsyncBody =
        extractFunctionBody(connectionText, "bool V1BLEClient::startAsyncConnect()");
    const std::string connectingWaitBody =
        extractFunctionBody(connectionText, "void V1BLEClient::processConnectingWait()");
    const std::string subscribeStepBody =
        extractFunctionBody(connectionText, "bool V1BLEClient::executeSubscribeStep()");
    const std::string wifiPriorityBody =
        extractFunctionBody(runtimeText, "void V1BLEClient::setWifiPriority(bool enabled)");
    const std::string followupBody =
        extractFunctionBody(followupText, "void V1BLEClient::processConnectedFollowup()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          connectionText.find("#include \"ble_log_rate_limit.h\""));
    TEST_ASSERT_EQUAL(std::string::npos, phyUpdateBody.find("Serial."));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          startAsyncBody.find("connectInitiationFailedLog"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          startAsyncBody.find("shouldLogBleConnectionEvent("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          connectingWaitBody.find("connectAttemptFailedLog"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          subscribeStepBody.find("subscribeFailServiceLog"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          wifiPriorityBody.find("BleLogRateLimitState wifiPriorityLog"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          wifiPriorityBody.find("if (shouldLog) Serial.println(\"[BLE] Stopping scan for WiFi priority mode\")"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          followupBody.find("logNonCriticalFollowupFailure("));
    TEST_ASSERT_EQUAL(std::string::npos,
                      followupBody.find("Serial.println(\"[BLE] Failed to request"));
}

void test_ble_queue_append_uses_resize_memcpy_and_resync_logs_are_time_gated() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/modules/ble/ble_queue_module.cpp");
    const std::filesystem::path headerSource =
        std::filesystem::path(projectRoot() + "/src/modules/ble/ble_queue_module.h");
    const std::string text = readFile(source);
    const std::string headerText = readFile(headerSource);
    const std::string appendBody =
        extractFunctionBody(text, "static size_t appendRxClamped");
    const std::string processBody =
        extractFunctionBody(text, "void BleQueueModule::process()");

    TEST_ASSERT_EQUAL(std::string::npos, appendBody.find("rxBuffer.insert("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          appendBody.find("rxBuffer.resize(oldSize + toCopy);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          appendBody.find("memcpy(rxBuffer.data() + oldSize, data, toCopy);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("BleLogRateLimitState tooLargeWarningLog_;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("BleLogRateLimitState missingEndWarningLog_;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          processBody.find("kBleResyncLogMinIntervalMs"));
}

void test_runtime_disconnected_state_no_longer_waits_for_connect_backoff() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "void V1BLEClient::process()");

    TEST_ASSERT_EQUAL(std::string::npos,
                      body.find("Still in backoff - don't scan yet"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      body.find("consecutiveConnectFailures_ > 0 && static_cast<int32_t>(now - nextConnectAllowedMs_) < 0"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("// Not connected_ - keep actively scanning for the V1."));
}

void test_connection_failures_no_longer_transition_to_backoff_state() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::string text = readFile(source);

    TEST_ASSERT_EQUAL(std::string::npos,
                      text.find("setBLEState(BLEState::BACKOFF"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      text.find("computeV1BleBackoffMs("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          text.find("setBLEState(BLEState::DISCONNECTED, \"connect timeout\")"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          text.find("setBLEState(BLEState::DISCONNECTED, \"all connect attempts failed\")"));
}

void test_wifi_priority_disable_no_longer_resumes_proxy_inline() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "void V1BLEClient::setWifiPriority(bool enabled)");

    TEST_ASSERT_EQUAL(std::string::npos,
                      body.find("StartWifiPriorityResume"));
    TEST_ASSERT_EQUAL_UINT64(0, countOccurrences(body, "proxyAdvertisingStartMs_ = static_cast<uint32_t>(millis()) + 500"));
}

void test_scan_stopping_uses_instance_owned_results_cleared_state() {
    const std::filesystem::path runtimeSource =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::filesystem::path clientSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.cpp");
    const std::string runtimeText = readFile(runtimeSource);
    const std::string clientText = readFile(clientSource);
    const std::string processBody = extractFunctionBody(runtimeText, "void V1BLEClient::process()");
    const std::string setStateBody = extractFunctionBody(clientText, "void V1BLEClient::setBLEState");
    const std::string cleanupBody = extractFunctionBody(clientText, "void V1BLEClient::cleanupConnection()");

    TEST_ASSERT_EQUAL(std::string::npos, processBody.find("static bool resultsCleared"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, processBody.find("scanStopResultsCleared_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, setStateBody.find("scanStopResultsCleared_ = false"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, cleanupBody.find("scanStopResultsCleared_ = false"));
}

void test_destructor_clears_instance_ptr_only_for_active_instance() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_client.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "V1BLEClient::~V1BLEClient()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("if (instancePtr == this)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("instancePtr = nullptr"));
}

void test_connect_to_server_removes_unused_addr_type_local() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "bool V1BLEClient::connectToServer()");

    TEST_ASSERT_EQUAL(std::string::npos, body.find("addrType"));
}

void test_ble_mutex_trylocks_use_semaphore_guard_in_runtime_and_callbacks() {
    const std::filesystem::path runtimeSource =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::filesystem::path connectionSource =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::string runtimeText = readFile(runtimeSource);
    const std::string connectionText = readFile(connectionSource);

    TEST_ASSERT_EQUAL(std::string::npos, runtimeText.find("xSemaphoreTake(bleMutex_"));
    TEST_ASSERT_EQUAL(std::string::npos, runtimeText.find("xSemaphoreGive(bleMutex_"));
    TEST_ASSERT_EQUAL(std::string::npos, connectionText.find("xSemaphoreTake(instancePtr->bleMutex_"));
    TEST_ASSERT_EQUAL(std::string::npos, connectionText.find("xSemaphoreGive(instancePtr->bleMutex_"));
    TEST_ASSERT_EQUAL(std::string::npos, connectionText.find("xSemaphoreTake(bleClient->bleMutex_"));
    TEST_ASSERT_EQUAL(std::string::npos, connectionText.find("xSemaphoreGive(bleClient->bleMutex_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, runtimeText.find("SemaphoreGuard lock(bleMutex_, 0)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, connectionText.find("SemaphoreGuard lock(instancePtr->bleMutex_, 0)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, connectionText.find("SemaphoreGuard lock(bleClient->bleMutex_, 0)"));
}

void test_connected_flag_uses_explicit_atomic_load_store() {
    const std::filesystem::path clientSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.cpp");
    const std::filesystem::path runtimeSource =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::filesystem::path connectionSource =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::filesystem::path proxySource =
        std::filesystem::path(projectRoot() + "/src/ble_proxy.cpp");
    const std::string clientText = readFile(clientSource);
    const std::string runtimeText = readFile(runtimeSource);
    const std::string connectionText = readFile(connectionSource);
    const std::string proxyText = readFile(proxySource);

    TEST_ASSERT_NOT_EQUAL(std::string::npos, clientText.find("connected_.load(std::memory_order_acquire)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, runtimeText.find("connected_.store(false, std::memory_order_relaxed)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, runtimeText.find("connected_.store(true, std::memory_order_relaxed)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, connectionText.find("connected_.store(true, std::memory_order_relaxed)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, connectionText.find("connected_.store(false, std::memory_order_release)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, proxyText.find("connected_.load(std::memory_order_relaxed)"));
}

void test_ble_timing_state_and_rssi_caches_use_uint32() {
    const std::filesystem::path headerSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.h");
    const std::filesystem::path clientSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.cpp");
    const std::string headerText = readFile(headerSource);
    const std::string clientText = readFile(clientSource);

    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t lastScanStart_;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t stateEnteredMs_ = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t scanStopRequestedMs_ = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t connectStartMs_ = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t nextConnectAllowedMs_ = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t proxyAdvertisingStartMs_ = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t proxyFastAdvertisingUntilMs_ = 0;"));
    TEST_ASSERT_EQUAL(std::string::npos, headerText.find("uint32_t proxyAdvertisingWindowStartMs_ = 0;"));
    TEST_ASSERT_EQUAL(std::string::npos, headerText.find("uint32_t proxyAdvertisingRetryAtMs_ = 0;"));
    TEST_ASSERT_EQUAL(std::string::npos, headerText.find("unsigned long lastScanStart_"));
    TEST_ASSERT_EQUAL(std::string::npos, headerText.find("unsigned long stateEnteredMs_"));
    TEST_ASSERT_EQUAL(std::string::npos, headerText.find("unsigned long connectStartMs_"));
    TEST_ASSERT_EQUAL(std::string::npos, headerText.find("unsigned long nextConnectAllowedMs_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, clientText.find("static uint32_t s_lastV1RssiQueryMs = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, clientText.find("static constexpr uint32_t RSSI_QUERY_INTERVAL_MS = 2000;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, clientText.find("static uint32_t s_lastProxyRssiQueryMs = 0;"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ble_connection_log_rate_limit_allows_first_then_bounds_burst);
    RUN_TEST(test_async_connect_does_not_delete_bond);
    RUN_TEST(test_disconnect_callback_still_defers_bond_heal);
    RUN_TEST(test_disconnect_callback_no_longer_stops_proxy_advertising_inline);
    RUN_TEST(test_v1_connection_event_timestamp_is_written_on_connect_and_disconnect);
    RUN_TEST(test_verify_push_edge_state_is_tracked_in_header_and_commands);
    RUN_TEST(test_proxy_control_wrappers_are_exposed_and_delegate_to_existing_proxy_logic);
    RUN_TEST(test_proxy_app_mode_runtime_toggle_and_local_write_contract);
    RUN_TEST(test_proxy_server_is_preinitialized_before_runtime_toggle);
    RUN_TEST(test_explicit_proxy_mode_gates_proxy_policy_and_drops_obd);
    RUN_TEST(test_connection_cycle_state_setter_updates_diagnostic_cache);
    RUN_TEST(test_app_disconnect_no_longer_reschedules_proxy_inline);
    RUN_TEST(test_manual_obd_preempt_disconnects_proxy_from_main_loop);
    RUN_TEST(test_connected_followup_no_longer_schedules_proxy_advertising);
    RUN_TEST(test_runtime_uses_coordinator_proxy_policy_instead_of_proxy_windows);
    RUN_TEST(test_runtime_proxy_guard_keeps_non_coordinator_safety_clauses);
    RUN_TEST(test_runtime_process_consolidates_proxy_and_state_machine_millis_sample);
    RUN_TEST(test_noncritical_ble_connection_logs_are_rate_limited_or_callback_disabled);
    RUN_TEST(test_ble_queue_append_uses_resize_memcpy_and_resync_logs_are_time_gated);
    RUN_TEST(test_runtime_disconnected_state_no_longer_waits_for_connect_backoff);
    RUN_TEST(test_connection_failures_no_longer_transition_to_backoff_state);
    RUN_TEST(test_wifi_priority_disable_no_longer_resumes_proxy_inline);
    RUN_TEST(test_scan_stopping_uses_instance_owned_results_cleared_state);
    RUN_TEST(test_destructor_clears_instance_ptr_only_for_active_instance);
    RUN_TEST(test_connect_to_server_removes_unused_addr_type_local);
    RUN_TEST(test_ble_mutex_trylocks_use_semaphore_guard_in_runtime_and_callbacks);
    RUN_TEST(test_connected_flag_uses_explicit_atomic_load_store);
    RUN_TEST(test_ble_timing_state_and_rssi_caches_use_uint32);
    return UNITY_END();
}
