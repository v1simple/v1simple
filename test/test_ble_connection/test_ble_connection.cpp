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

// Collapse every run of whitespace to a single space so that assertions about
// production source survive clang-format re-wrapping. Without this, a statement
// like `if (x) foo();` that clang-format legally splits across two lines
// (AllowShortIfStatementsOnASingleLine: false) would stop matching even though
// the code is semantically identical. Normalizing both haystack and needle keeps
// the assertion exactly as strong -- the tokens must still all be present, in
// order -- while making it insensitive to line breaks and indentation.
std::string normalizeWhitespace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool inSpace = false;
    for (const char ch : text) {
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            if (!inSpace) {
                out.push_back(' ');
                inSpace = true;
            }
        } else {
            out.push_back(ch);
            inSpace = false;
        }
    }
    return out;
}

// Format-tolerant substring search: true when `needle` occurs in `haystack`
// ignoring only whitespace layout.
bool containsNormalized(const std::string& haystack, const std::string& needle) {
    return normalizeWhitespace(haystack).find(normalizeWhitespace(needle)) != std::string::npos;
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

void test_disconnect_reason_is_deferred_then_logged_from_main_loop() {
    const std::filesystem::path headerSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.h");
    const std::filesystem::path connectionSource =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::filesystem::path runtimeSource =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::string headerText = readFile(headerSource);
    const std::string connectionText = readFile(connectionSource);
    const std::string runtimeText = readFile(runtimeSource);
    const std::string disconnectBody =
        extractFunctionBody(connectionText, "void V1BLEClient::ClientCallbacks::onDisconnect");
    const std::string processBody = extractFunctionBody(runtimeText, "void V1BLEClient::process()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("std::atomic<int> pendingDisconnectReason_{0};"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          disconnectBody.find("pendingDisconnectReason_.store(reason"));
    TEST_ASSERT_EQUAL(std::string::npos, disconnectBody.find("Serial.printf"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          processBody.find("pendingDisconnectReason_.exchange(0"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          processBody.find("Applying V1 disconnect reason=%d eventMs=%lu"));
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
    // Whitespace-normalized: clang-format may split this guarded log onto two lines.
    // The assertion still requires the println to be gated by `shouldLog`.
    TEST_ASSERT_TRUE_MESSAGE(
        containsNormalized(wifiPriorityBody,
                           "if (shouldLog) Serial.println(\"[BLE] Stopping scan for WiFi priority mode\")"),
        "scan-stop log must stay rate-limited behind shouldLog");
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
                          text.find("beginClientQuiesce(\"connect timeout\")"));
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
    TEST_ASSERT_EQUAL(std::string::npos, connectionText.find("SemaphoreGuard lock(instancePtr->bleMutex_, 0)"));
    TEST_ASSERT_EQUAL(std::string::npos, connectionText.find("SemaphoreGuard lock(bleClient->bleMutex_, 0)"));
}

void test_runtime_process_retries_deferred_proxy_queue_release() {
    const std::string runtimeText =
        readFile(std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp"));
    const std::string processBody = extractFunctionBody(runtimeText, "void V1BLEClient::process()");

    const size_t pendingGate = processBody.find("proxyQueueReleasePending_.load(std::memory_order_acquire)");
    const size_t finalizeCall = processBody.find("tryFinalizeProxyQueueRelease()");
    const size_t callbackDrain = processBody.find("pendingConnectStateUpdate_");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, pendingGate);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, finalizeCall);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, callbackDrain);
    TEST_ASSERT_TRUE(pendingGate < finalizeCall);
    TEST_ASSERT_TRUE(finalizeCall < callbackDrain);
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
    TEST_ASSERT_NOT_EQUAL(std::string::npos, connectionText.find("connected_.store(true, std::memory_order_release)"));
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

// Scan-callback ownership: the NimBLE scan object is a shared singleton; the
// OBD pair scan legitimately swaps in its own callbacks (obd_ble_client.cpp),
// so every V1 scan start must re-assert V1's callbacks or the V1 can never be
// rediscovered after an OBD scan (wedge fixed 2026-07-09 review).
void test_v1_scan_starts_reassert_scan_callback_ownership() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::string text = readFile(source);

    const std::string reassert = "pScan->setScanCallbacks(pScanCallbacks_.get());";

    const std::string processBody = extractFunctionBody(text, "void V1BLEClient::process()");
    const size_t processReassert = processBody.find(reassert);
    const size_t processStart = processBody.find("bool started = pScan->start(SCAN_DURATION");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, processReassert);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, processStart);
    TEST_ASSERT_TRUE(processReassert < processStart);

    const std::string manualBody = extractFunctionBody(text, "void V1BLEClient::startScanning()");
    const size_t manualReassert = manualBody.find(reassert);
    const size_t manualStart = manualBody.find("bool started = pScan->start(SCAN_DURATION");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, manualReassert);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, manualStart);
    TEST_ASSERT_TRUE(manualReassert < manualStart);
}

// SCANNING must not depend solely on onScanEnd arriving on our callback object:
// a missed/foreign scan-end edge previously wedged reconnection until reboot.
void test_scanning_state_has_scan_watchdog_recovery() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "void V1BLEClient::process()");

    const size_t watchdog = body.find("setBLEState(BLEState::DISCONNECTED, \"scan watchdog\");");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, watchdog);
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("(now - lastScanStart_) > (SCAN_DURATION + 5000u)"));
}

// A failed OBD scan start means onScanEnd never fires; RPA resolution must be
// re-enabled on that path or it leaks disabled stack-wide.
void test_obd_scan_start_failure_reenables_privacy_resolution() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/modules/obd/obd_ble_client.cpp");
    const std::string text = readFile(source);
    const std::string body =
        extractFunctionBody(text, "bool ObdBleClient::startScan(ObdRuntimeModule* parent, int8_t minRssi)");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("ble_hs_pvcy_set_resolve_enabled(0);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("if (!started) {"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("ble_hs_pvcy_set_resolve_enabled(1);"));
}

// The deferred bond-delete address is a plain NimBLEAddress written on the
// NimBLE host task; both sides must copy under pendingAddrMux so a second
// disconnect cannot tear the in-flight address.
void test_deferred_bond_delete_address_guarded_by_pending_addr_mux() {
    const std::filesystem::path runtimeSource =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::string runtimeText = readFile(runtimeSource);
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          runtimeText.find("const NimBLEAddress addrToDelete = pendingDeleteBondAddr_;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, runtimeText.find("NimBLEDevice::isBonded(addrToDelete)"));

    const std::filesystem::path connectionSource =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::string connectionText = readFile(connectionSource);
    const std::string disconnectBody = extractFunctionBody(
        connectionText, "void V1BLEClient::ClientCallbacks::onDisconnect(NimBLEClient* pClient_, int reason)");
    const size_t enter = disconnectBody.find("portENTER_CRITICAL(&pendingAddrMux);");
    const size_t write = disconnectBody.find("instancePtr->pendingDeleteBondAddr_ = peerAddr;");
    const size_t exit = disconnectBody.find("portEXIT_CRITICAL(&pendingAddrMux);");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, enter);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, write);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, exit);
    TEST_ASSERT_TRUE(enter < write);
    TEST_ASSERT_TRUE(write < exit);
}

void test_disconnect_callback_defers_all_remote_handle_cleanup_to_main_loop() {
    const std::string connectionText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp"));
    const std::string body = extractFunctionBody(
        connectionText, "void V1BLEClient::ClientCallbacks::onDisconnect");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("pendingDisconnectCleanup_.store(true"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("pRemoteService_"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("pDisplayDataChar_"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("pCommandChar_"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("setBLEState("));
}

void test_connect_callback_never_publishes_connected_directly() {
    const std::string connectionText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp"));
    const std::string body = extractFunctionBody(
        connectionText, "void V1BLEClient::ClientCallbacks::onConnect");

    TEST_ASSERT_EQUAL(std::string::npos, body.find("connected_.store(true"));
    const size_t generationCapture = body.find("const uint32_t callbackGeneration");
    const size_t acceptanceGate = body.find("sessionPublicationGate_.accepts(callbackGeneration)");
    const size_t generationTag =
        body.find("pendingConnectStateGeneration_.store(");
    const size_t deferredEdge = body.find("pendingConnectStateUpdate_.store(true");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, generationCapture);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, acceptanceGate);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, generationTag);
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          deferredEdge);
    TEST_ASSERT_TRUE(generationCapture < acceptanceGate);
    TEST_ASSERT_TRUE(acceptanceGate < generationTag);
    TEST_ASSERT_TRUE(generationTag < deferredEdge);
}

void test_deferred_connect_edge_is_generation_scoped_and_retired_on_quiesce() {
    const std::string runtimeText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp"));
    const std::string connectionText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp"));
    const std::string runtimeBody =
        extractFunctionBody(runtimeText, "void V1BLEClient::process()");
    const std::string quiesceBody =
        extractFunctionBody(connectionText, "void V1BLEClient::processClientQuiesce()");

    const size_t edgeLoad = runtimeBody.find("pendingConnectStateGeneration_.load");
    const size_t generationCheck = runtimeBody.find(
        "edgeGeneration == sessionGeneration_.load(std::memory_order_acquire)");
    const size_t gateCheck =
        runtimeBody.find("sessionPublicationGate_.accepts(edgeGeneration)");
    const size_t publish = runtimeBody.find("connected_.store(edgeStillAccepted");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, edgeLoad);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, generationCheck);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, gateCheck);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, publish);
    TEST_ASSERT_TRUE(edgeLoad < generationCheck);
    TEST_ASSERT_TRUE(generationCheck < publish);
    TEST_ASSERT_TRUE(gateCheck < publish);

    const size_t cleanup = quiesceBody.find("cleanupConnection();");
    const size_t retireEdge =
        quiesceBody.find("pendingConnectStateUpdate_.store(false", cleanup);
    const size_t retireGeneration =
        quiesceBody.find("pendingConnectStateGeneration_.store(0", cleanup);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, cleanup);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, retireEdge);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, retireGeneration);
    TEST_ASSERT_TRUE(cleanup < retireEdge);
    TEST_ASSERT_TRUE(retireEdge < retireGeneration);
}

void test_subscribe_completion_revalidates_session_before_publication() {
    const std::string connectionText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp"));
    const std::string body =
        extractFunctionBody(connectionText, "void V1BLEClient::processSubscribing()");

    const size_t generationCapture =
        body.find("const uint32_t completedGeneration = activeDiscoveryGeneration_");
    const size_t gateClaim =
        body.find("sessionPublicationGate_.claim(completedGeneration)");
    const size_t connectedPublish = body.find("connected_.store(true");
    const size_t postPublishCheck =
        body.find("if (!sessionStillAccepted())", connectedPublish);
    const size_t statePublish =
        body.find("setBLEState(BLEState::CONNECTED, \"subscribe complete\")");
    const size_t callbackGate =
        body.find("connectImmediateCallback_ && sessionStillAccepted()");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, generationCapture);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, gateClaim);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, connectedPublish);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, postPublishCheck);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, statePublish);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, callbackGate);
    TEST_ASSERT_TRUE(generationCapture < connectedPublish);
    // The optimistic publication must precede the RMW claim. This makes a
    // callback close either win before the claim (and force retraction) or
    // occur after the claim with its false store ordered last.
    TEST_ASSERT_TRUE(connectedPublish < gateClaim);
    TEST_ASSERT_TRUE(gateClaim < postPublishCheck);
    TEST_ASSERT_TRUE(postPublishCheck < statePublish);
    TEST_ASSERT_TRUE(statePublish < callbackGate);
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        body.find("beginClientQuiesce(\"subscribe publication invalidated\")"));
}

void test_scan_target_callbacks_publish_only_deferred_edges() {
    const std::string connectionText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp"));
    const std::string runtimeText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp"));
    const std::string callbackBody = extractFunctionBody(
        connectionText, "void V1BLEClient::ScanCallbacks::onResult");
    const std::string scanEndBody = extractFunctionBody(
        connectionText, "void V1BLEClient::ScanCallbacks::onScanEnd");
    const std::string runtimeBody =
        extractFunctionBody(runtimeText, "void V1BLEClient::process()");

    const size_t targetPublish = callbackBody.find(
        "pendingScanTargetUpdate_.store(true, std::memory_order_release)");
    const size_t scanStop = callbackBody.find("pScan->stop()");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, targetPublish);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, scanStop);
    TEST_ASSERT_TRUE(targetPublish < scanStop);
    TEST_ASSERT_EQUAL(std::string::npos, callbackBody.find("bleState_"));
    TEST_ASSERT_EQUAL(std::string::npos, callbackBody.find("setBLEState("));
    TEST_ASSERT_EQUAL(std::string::npos, callbackBody.find("SemaphoreGuard"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        scanEndBody.find("pendingScanEndUpdate_.store(true, std::memory_order_release)"));
    TEST_ASSERT_EQUAL(std::string::npos, scanEndBody.find("bleState_"));
    TEST_ASSERT_EQUAL(std::string::npos, scanEndBody.find("setBLEState("));
    TEST_ASSERT_EQUAL(std::string::npos, scanEndBody.find("SemaphoreGuard"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        runtimeBody.find("!pendingScanTargetUpdate_.load(std::memory_order_acquire)"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        runtimeBody.find("if (havePending && bleState_ == BLEState::SCANNING)"));
    TEST_ASSERT_EQUAL(
        std::string::npos,
        runtimeBody.find("if (havePending && bleState_ != BLEState::QUIESCING"));
}

void test_notify_callback_drops_after_quiesce_or_missing_mapping() {
    const std::string connectionText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp"));
    const std::string body = extractFunctionBody(
        connectionText, "void V1BLEClient::notifyCallback");

    const size_t acceptGate =
        body.find("acceptClientCallbacks_.load(std::memory_order_acquire)");
    const size_t mappingLoad = body.find("notifyShortChar_.load");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, acceptGate);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, mappingLoad);
    TEST_ASSERT_TRUE(acceptGate < mappingLoad);
    TEST_ASSERT_EQUAL(std::string::npos, body.find("pChar->getUUID()"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("charId = 0xB2CE"));
}

void test_discovery_completion_is_generation_scoped_and_quiesced_before_reconnect() {
    const std::string headerText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_client.h"));
    const std::string connectionText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp"));
    const std::string runtimeText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp"));
    const std::string connectBody =
        extractFunctionBody(connectionText, "bool V1BLEClient::connectToServer()");
    const std::string discoverBody =
        extractFunctionBody(connectionText, "void V1BLEClient::processDiscovering()");
    const std::string quiesceBody =
        extractFunctionBody(connectionText, "void V1BLEClient::processClientQuiesce()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("struct DiscoveryTaskContext"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("uint32_t generation = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          connectBody.find("discoveryTaskRunning_.load(std::memory_order_acquire)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          discoverBody.find("completedGeneration != activeDiscoveryGeneration_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          discoverBody.find("completedGeneration != sessionGeneration_.load"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          discoverBody.find("beginClientQuiesce(\"discovery timeout\")"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      discoverBody.find("setBLEState(BLEState::DISCONNECTED, \"discovery timeout\")"));

    const size_t waitDiscovery = quiesceBody.find("discoveryTaskRunning_.load(std::memory_order_acquire)");
    const size_t cleanup = quiesceBody.find("cleanupConnection();");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, waitDiscovery);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, cleanup);
    TEST_ASSERT_TRUE(waitDiscovery < cleanup);
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          runtimeText.find("case BLEState::QUIESCING:"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          runtimeText.find("processClientQuiesce();"));
}

void test_quiescence_retries_then_fails_closed_instead_of_deadlocking_forever() {
    const std::string headerText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_client.h"));
    const std::string connectionText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp"));
    const std::string body =
        extractFunctionBody(connectionText, "void V1BLEClient::processClientQuiesce()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("QUIESCE_RETRY_MS = 250"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          headerText.find("QUIESCE_FATAL_TIMEOUT_MS = 15000"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("pClient_->cancelConnect();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("pClient_->disconnect();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("bleQuiesceDeadlineExpired("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("quiesceTimeoutRecoveryCount_.fetch_add"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("ESP.restart();"));

    const size_t fatalGate = body.find("bleQuiesceDeadlineExpired(");
    const size_t callbackWaitBranch = body.find(
        "if (waitingForConnectCancel || waitingForDiscovery || waitingForDisconnect) {");
    const size_t connectedRetry = body.find("if (clientStillConnected) {");
    const size_t restart = body.find("ESP.restart();");
    const size_t cleanup = body.find("cleanupConnection();");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, callbackWaitBranch);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, connectedRetry);
    TEST_ASSERT_TRUE(fatalGate < restart);
    TEST_ASSERT_TRUE(restart < cleanup);
    TEST_ASSERT_TRUE(fatalGate < callbackWaitBranch);
    TEST_ASSERT_TRUE(fatalGate < connectedRetry);
}

void test_disconnect_callback_rejects_old_connection_handle() {
    const std::string connectionText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp"));
    const std::string body = extractFunctionBody(
        connectionText, "void V1BLEClient::ClientCallbacks::onDisconnect");

    const size_t readHandle = body.find("const uint16_t callbackHandle = pClient_->getConnHandle();");
    const size_t reject = body.find("callbackHandle != expectedHandle");
    const size_t publishCleanup = body.find("pendingDisconnectCleanup_.store(true");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, readHandle);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, reject);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, publishCleanup);
    TEST_ASSERT_TRUE(readHandle < reject);
    TEST_ASSERT_TRUE(reject < publishCleanup);
}

void test_discovery_task_publishes_generation_and_exit_stack_before_releasing_owner() {
    const std::string connectionText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp"));
    const std::string body =
        extractFunctionBody(connectionText, "void V1BLEClient::discoveryTaskFunc(void* param)");

    const size_t stackSample = body.find("uxTaskGetStackHighWaterMark(nullptr)");
    const size_t generationPublish =
        body.find("discoveryCompletedGeneration_.store(context.generation");
    const size_t releaseOwner =
        body.find("discoveryTaskRunning_.store(false, std::memory_order_release)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, stackSample);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, generationPublish);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, releaseOwner);
    TEST_ASSERT_TRUE(stackSample < releaseOwner);
    TEST_ASSERT_TRUE(generationPublish < releaseOwner);
}

void test_hard_reset_is_nonblocking_and_uses_quiescence_state() {
    const std::string clientText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_client.cpp"));
    const std::string body =
        extractFunctionBody(clientText, "void V1BLEClient::hardResetBLEClient()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("beginClientQuiesce(\"hard reset requested\", true)"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("while ("));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("vTaskDelay("));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("500"));
}

void test_command_write_uses_one_main_loop_owned_handle_snapshot() {
    const std::string commandsText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_commands.cpp"));
    const std::string body = extractFunctionBody(
        commandsText, "SendResult V1BLEClient::sendCommandWithResult");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          body.find("NimBLERemoteCharacteristic* const commandChar = pCommandChar_;"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("pCommandChar_->"));
}

void test_notify_queue_full_path_drops_incoming_without_evicting_queue_head() {
    const std::string headerText = readFile(
        std::filesystem::path(projectRoot() + "/src/modules/ble/ble_queue_module.h"));
    const std::string queueText = readFile(
        std::filesystem::path(projectRoot() + "/src/modules/ble/ble_queue_module.cpp"));
    const std::string body =
        extractFunctionBody(queueText, "void BleQueueModule::onNotify");

    TEST_ASSERT_EQUAL(std::string::npos, headerText.find("BLEDataPacket dropScratch_{};"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("xQueueReceive(queueHandle_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("PERF_INC(queueDrops)"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("BLEDataPacket dropped"));
    TEST_ASSERT_EQUAL_UINT64(1, countOccurrences(body, "BLEDataPacket pkt"));
}

void test_session_open_boundary_precedes_subscription_work() {
    const std::string runtimeText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp"));
    const std::string processBody = extractFunctionBody(runtimeText, "void V1BLEClient::process()");

    const size_t acceptedEdge = processBody.find("connected_.store(edgeStillAccepted");
    const size_t openBoundary = processBody.find("sessionOpenedCallback_(edgeGeneration)");
    const size_t stateMachine = processBody.find("switch (bleState_)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, acceptedEdge);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, openBoundary);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, stateMachine);
    TEST_ASSERT_TRUE(acceptedEdge < openBoundary);
    TEST_ASSERT_TRUE(openBoundary < stateMachine);
}

void test_session_close_boundary_follows_generation_invalidation_once() {
    const std::string connectionText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp"));
    const std::string quiesceBody = extractFunctionBody(
        connectionText, "void V1BLEClient::beginClientQuiesce");
    const std::string disconnectBody = extractFunctionBody(
        connectionText, "void V1BLEClient::ClientCallbacks::onDisconnect");

    const size_t closeGate = quiesceBody.find("sessionPublicationGate_.close();");
    const size_t generationStore = quiesceBody.find("sessionGeneration_.store(nextGeneration");
    const size_t closeBoundary = quiesceBody.find("sessionClosedCallback_(nextGeneration)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, closeGate);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, generationStore);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, closeBoundary);
    TEST_ASSERT_TRUE(closeGate < generationStore);
    TEST_ASSERT_TRUE(generationStore < closeBoundary);
    TEST_ASSERT_EQUAL_UINT64(1, countOccurrences(quiesceBody, "sessionClosedCallback_("));
    TEST_ASSERT_EQUAL(std::string::npos, disconnectBody.find("sessionClosedCallback_("));
}

void test_notify_callback_propagates_only_revalidated_session_generation() {
    const std::string connectionText = readFile(
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp"));
    const std::string notifyBody = extractFunctionBody(
        connectionText, "void V1BLEClient::notifyCallback");

    const size_t generationCapture = notifyBody.find("const uint32_t callbackGeneration");
    const size_t generationRecheck = notifyBody.find(
        "sessionGeneration_.load(std::memory_order_acquire) == callbackGeneration");
    const size_t dataPublish = notifyBody.find(
        "dataCallback_(pData, length, charId, callbackGeneration)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, generationCapture);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, generationRecheck);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, dataPublish);
    TEST_ASSERT_TRUE(generationCapture < generationRecheck);
    TEST_ASSERT_TRUE(generationRecheck < dataPublish);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ble_connection_log_rate_limit_allows_first_then_bounds_burst);
    RUN_TEST(test_async_connect_does_not_delete_bond);
    RUN_TEST(test_disconnect_callback_still_defers_bond_heal);
    RUN_TEST(test_disconnect_reason_is_deferred_then_logged_from_main_loop);
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
    RUN_TEST(test_runtime_process_retries_deferred_proxy_queue_release);
    RUN_TEST(test_connected_flag_uses_explicit_atomic_load_store);
    RUN_TEST(test_ble_timing_state_and_rssi_caches_use_uint32);
    RUN_TEST(test_v1_scan_starts_reassert_scan_callback_ownership);
    RUN_TEST(test_scanning_state_has_scan_watchdog_recovery);
    RUN_TEST(test_obd_scan_start_failure_reenables_privacy_resolution);
    RUN_TEST(test_deferred_bond_delete_address_guarded_by_pending_addr_mux);
    RUN_TEST(test_disconnect_callback_defers_all_remote_handle_cleanup_to_main_loop);
    RUN_TEST(test_connect_callback_never_publishes_connected_directly);
    RUN_TEST(test_deferred_connect_edge_is_generation_scoped_and_retired_on_quiesce);
    RUN_TEST(test_subscribe_completion_revalidates_session_before_publication);
    RUN_TEST(test_scan_target_callbacks_publish_only_deferred_edges);
    RUN_TEST(test_notify_callback_drops_after_quiesce_or_missing_mapping);
    RUN_TEST(test_discovery_completion_is_generation_scoped_and_quiesced_before_reconnect);
    RUN_TEST(test_quiescence_retries_then_fails_closed_instead_of_deadlocking_forever);
    RUN_TEST(test_disconnect_callback_rejects_old_connection_handle);
    RUN_TEST(test_discovery_task_publishes_generation_and_exit_stack_before_releasing_owner);
    RUN_TEST(test_hard_reset_is_nonblocking_and_uses_quiescence_state);
    RUN_TEST(test_command_write_uses_one_main_loop_owned_handle_snapshot);
    RUN_TEST(test_notify_queue_full_path_drops_incoming_without_evicting_queue_head);
    RUN_TEST(test_session_open_boundary_precedes_subscription_work);
    RUN_TEST(test_session_close_boundary_follows_generation_invalidation_once);
    RUN_TEST(test_notify_callback_propagates_only_revalidated_session_generation);
    return UNITY_END();
}
