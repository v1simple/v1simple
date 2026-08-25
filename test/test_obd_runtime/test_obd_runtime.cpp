/**
 * Behavioral characterization of the production OBD runtime/state machine.
 * Tests drive public callbacks, runtime updates, and transport-boundary outcomes;
 * they do not replace the state machine with a test model.
 */
#include <unity.h>

#include <cmath>
#include <cstring>

#include "../mocks/Arduino.h"
#include "../../src/modules/gps/gps_runtime_status.h"
#include "../../src/modules/obd/obd_elm327_parser.cpp"
#include "../../src/modules/obd/obd_runtime_module.cpp"
#include "../../src/modules/obd/obd_runtime_state_machine.cpp"
#include "../../src/modules/obd/obd_runtime_commands.cpp"
#include "../../src/modules/obd/obd_runtime_transport.cpp"

// SpeedSourceSelector deliberately accepts fixture-provided runtime types in
// UNIT_TEST builds. OBD is the real runtime above; GPS is disabled in this
// suite and only needs its compile-time surface.
class GpsRuntimeModule {
  public:
    bool getFreshSpeed(uint32_t, float&, uint32_t&) const { return false; }
    GpsRuntimeStatus snapshot(uint32_t) const { return {}; }
};

#include "../../src/modules/speed/speed_source_selector.cpp"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

namespace {

constexpr const char* kSavedAddress = "A4:C1:38:00:11:22";

struct Fixture {
    ObdRuntimeModule runtime;
    ObdBleContext context;
    uint32_t nowMs = 10;

    Fixture() {
        context.bootReady = true;
        context.v1Connected = true;
        context.bleScanIdle = true;
        context.obdScanAllowed = true;
        context.obdConnectAllowed = true;
        context.obdRetryAllowed = true;
    }

    void begin(bool enabled = true, const char* savedAddress = kSavedAddress) {
        runtime.begin(nullptr, enabled, savedAddress, 0, -80);
    }

    void update(uint32_t atMs) {
        nowMs = atMs;
        mockMillis = atMs;
        runtime.update(atMs, context);
    }

    void advance(uint32_t deltaMs = 1) { update(nowMs + deltaMs); }

    void connectThroughDiscovery() {
        TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, runtime.getState());
        update(nowMs);
        TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, runtime.getState());

        advance();
        TEST_ASSERT_EQUAL_UINT32(1, runtime.getConnectCallCountForTest());
        runtime.setTestBleConnected(true);
        advance();
        TEST_ASSERT_EQUAL(ObdConnectionState::DISCOVERING, runtime.getState());

        advance(obd::POST_CONNECT_SETTLE_MS);
        TEST_ASSERT_EQUAL_UINT32(1, runtime.getDiscoverCallCountForTest());
        advance();
        advance();
        TEST_ASSERT_EQUAL(ObdConnectionState::AT_INIT, runtime.getState());
    }

    void respondToActiveCommand(const char* response) {
        advance(); // consume the synchronous test transport's write result
        runtime.onBleData(reinterpret_cast<const uint8_t*>(response), strlen(response));
        advance();
    }

    void finishInit() {
        size_t commands = 0;
        while (runtime.getState() == ObdConnectionState::AT_INIT && commands < 12) {
            advance(obd::POST_SUBSCRIBE_SETTLE_MS);
            const ObdRuntimeStatus status = runtime.snapshot(nowMs);
            if (status.commandInFlight == ObdCommandKind::NONE) {
                continue;
            }
            const char* const command = runtime.getLastCommandForTest();
            const bool resetCommand = strncmp(command, "ATZ", 3) == 0;
            respondToActiveCommand(resetCommand ? "ELM327 v1.5\r>" : "OK\r>");
            commands++;
        }
        TEST_ASSERT_EQUAL_UINT32(obd::COLD_INIT_COMMAND_COUNT, commands);
        TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, runtime.getState());
    }

    void bootToPolling() {
        begin();
        connectThroughDiscovery();
        finishInit();
    }

    uint32_t startSpeedCommand(bool deferTransport = false) {
        if (deferTransport) {
            runtime.deferNextTransportResultForTest();
        }
        advance();
        TEST_ASSERT_EQUAL(ObdCommandKind::SPEED, runtime.snapshot(nowMs).commandInFlight);
        TEST_ASSERT_EQUAL_STRING(obd::SPEED_POLL_CMD, runtime.getLastCommandForTest());
        const uint32_t issuedMs = nowMs;
        if (!deferTransport) {
            advance();
        }
        return issuedMs;
    }

    void sendResponse(const char* response, uint32_t deltaMs = 1) {
        runtime.onBleData(reinterpret_cast<const uint8_t*>(response), strlen(response));
        advance(deltaMs);
    }
};

} // namespace

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
}
void tearDown() {}

void test_initial_connection_runs_discovery_init_and_acquires_speed() {
    Fixture fixture;
    fixture.bootToPolling();

    fixture.startSpeedCommand();
    fixture.sendResponse("41 0D 64\r>");

    const ObdRuntimeStatus status = fixture.runtime.snapshot(fixture.nowMs);
    TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, status.state);
    TEST_ASSERT_TRUE(status.connected);
    TEST_ASSERT_TRUE(status.speedValid);
    TEST_ASSERT_EQUAL_UINT32(1, status.connectSuccesses);
    TEST_ASSERT_EQUAL_UINT32(1, status.pollCount);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 62.1371f, status.speedMph);
}

void test_manual_scan_times_out_to_idle_and_discards_candidate_session() {
    Fixture fixture;
    fixture.begin(true, "");
    TEST_ASSERT_TRUE(fixture.runtime.requestManualPairScan(fixture.nowMs));

    fixture.advance();
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, fixture.runtime.getState());
    TEST_ASSERT_EQUAL_UINT32(1, fixture.runtime.getStartScanCallCountForTest());

    fixture.advance(obd::SCAN_DURATION_MS);
    const ObdRuntimeStatus status = fixture.runtime.snapshot(fixture.nowMs);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, status.state);
    TEST_ASSERT_FALSE(status.manualScanPending);
    TEST_ASSERT_FALSE(status.savedAddressValid);
}

void test_connection_timeout_disconnects_and_enters_retry_state() {
    Fixture fixture;
    fixture.begin();
    fixture.update(fixture.nowMs);
    fixture.runtime.deferNextTransportResultForTest();
    fixture.advance();
    TEST_ASSERT_EQUAL_UINT32(1, fixture.runtime.getConnectCallCountForTest());

    fixture.update(10 + obd::CONNECT_TIMEOUT_MS);
    const ObdRuntimeStatus status = fixture.runtime.snapshot(fixture.nowMs);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, status.state);
    TEST_ASSERT_EQUAL(ObdFailureReason::CONNECT_TIMEOUT, status.lastFailure);
    TEST_ASSERT_EQUAL_UINT32(1, status.connectFailures);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.runtime.getDisconnectCallCountForTest());
}

void test_discovery_transport_timeout_is_classified_as_discovery_failure() {
    Fixture fixture;
    fixture.begin();
    fixture.update(fixture.nowMs);
    fixture.advance();
    fixture.runtime.setTestBleConnected(true);
    fixture.advance();
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCOVERING, fixture.runtime.getState());

    fixture.runtime.deferNextTransportResultForTest();
    fixture.advance(obd::POST_CONNECT_SETTLE_MS);
    fixture.runtime.completePendingTransportForTest(false, true);
    fixture.advance();

    const ObdRuntimeStatus status = fixture.runtime.snapshot(fixture.nowMs);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, status.state);
    TEST_ASSERT_EQUAL(ObdFailureReason::DISCOVERY, status.lastFailure);
    TEST_ASSERT_EQUAL_UINT32(1, status.connectFailures);
}

void test_speed_command_transport_timeout_is_not_misreported_as_parse_failure() {
    Fixture fixture;
    fixture.bootToPolling();
    fixture.startSpeedCommand(true);
    fixture.runtime.completePendingTransportForTest(false, true);
    fixture.advance();

    const ObdRuntimeStatus status = fixture.runtime.snapshot(fixture.nowMs);
    TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, status.state);
    TEST_ASSERT_EQUAL(ObdFailureReason::COMMAND_TIMEOUT, status.lastFailure);
    TEST_ASSERT_EQUAL_UINT32(1, status.pollErrors);
    TEST_ASSERT_FALSE(status.speedValid);
}

void test_empty_response_retries_alternate_write_mode_then_times_out() {
    Fixture fixture;
    fixture.bootToPolling();
    const uint32_t firstIssuedMs = fixture.startSpeedCommand();
    const bool firstWriteMode = fixture.runtime.getLastWriteWithResponseForTest();

    fixture.update(firstIssuedMs + obd::POLL_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(ObdCommandKind::SPEED, fixture.runtime.snapshot(fixture.nowMs).commandInFlight);
    TEST_ASSERT_NOT_EQUAL(firstWriteMode, fixture.runtime.getLastWriteWithResponseForTest());
    const uint32_t retryIssuedMs = fixture.nowMs;
    fixture.advance();
    fixture.update(retryIssuedMs + obd::POLL_TIMEOUT_MS);

    const ObdRuntimeStatus status = fixture.runtime.snapshot(fixture.nowMs);
    TEST_ASSERT_EQUAL(ObdFailureReason::COMMAND_TIMEOUT, status.lastFailure);
    TEST_ASSERT_EQUAL_UINT32(1, status.pollErrors);
    TEST_ASSERT_EQUAL(ObdCommandKind::NONE, status.commandInFlight);
}

void test_partial_response_has_no_separate_parse_deadline_and_uses_response_timeout() {
    Fixture fixture;
    fixture.bootToPolling();
    const uint32_t issuedMs = fixture.startSpeedCommand();
    fixture.sendResponse("41 0D", 100);
    TEST_ASSERT_EQUAL(ObdCommandKind::SPEED, fixture.runtime.snapshot(fixture.nowMs).commandInFlight);

    fixture.update(issuedMs + obd::POLL_TIMEOUT_MS);
    const ObdRuntimeStatus status = fixture.runtime.snapshot(fixture.nowMs);
    TEST_ASSERT_EQUAL(ObdFailureReason::COMMAND_TIMEOUT, status.lastFailure);
    TEST_ASSERT_EQUAL_UINT32(1, status.pollErrors);
    TEST_ASSERT_EQUAL(ObdCommandKind::NONE, status.commandInFlight);
}

void test_disconnect_clears_speed_and_next_admitted_update_reconnects() {
    Fixture fixture;
    fixture.bootToPolling();
    fixture.startSpeedCommand();
    fixture.sendResponse("41 0D 28\r>");
    TEST_ASSERT_TRUE(fixture.runtime.snapshot(fixture.nowMs).speedValid);

    fixture.runtime.setTestBleConnected(false);
    fixture.runtime.onBleDisconnect(520);
    fixture.advance();
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, fixture.runtime.getState());
    TEST_ASSERT_FALSE(fixture.runtime.snapshot(fixture.nowMs).speedValid);

    fixture.advance();
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, fixture.runtime.getState());
    fixture.advance();
    TEST_ASSERT_EQUAL_UINT32(2, fixture.runtime.getConnectCallCountForTest());
}

void test_old_session_response_queued_with_disconnect_cannot_seed_reconnect() {
    Fixture fixture;
    fixture.bootToPolling();
    fixture.startSpeedCommand();

    fixture.runtime.onBleData(reinterpret_cast<const uint8_t*>("41 0D 64\r>"), 10);
    fixture.runtime.onBleDisconnect(520);
    fixture.runtime.setTestBleConnected(false);
    fixture.advance();

    const ObdRuntimeStatus disconnected = fixture.runtime.snapshot(fixture.nowMs);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, disconnected.state);
    TEST_ASSERT_EQUAL_UINT32(0, disconnected.pollCount);
    TEST_ASSERT_FALSE(disconnected.speedValid);

    fixture.advance();
    fixture.advance();
    fixture.runtime.setTestBleConnected(true);
    fixture.advance();
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCOVERING, fixture.runtime.getState());
    TEST_ASSERT_EQUAL_UINT32(0, fixture.runtime.snapshot(fixture.nowMs).pollCount);
}

void test_real_speed_sample_is_primary_then_expires_and_invalidates_selection() {
    Fixture fixture;
    fixture.bootToPolling();
    fixture.startSpeedCommand();
    fixture.sendResponse("41 0D 50\r>");
    const uint32_t sampleMs = fixture.nowMs;

    SpeedSourceSelector selector;
    selector.begin(&fixture.runtime, true, nullptr, false);
    selector.update(sampleMs);
    SpeedSelection selected = selector.selectedSpeed();
    TEST_ASSERT_TRUE(selected.valid);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, selected.source);

    selector.update(sampleMs + obd::SPEED_MAX_AGE_MS);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, selector.selectedSpeed().source);

    fixture.update(sampleMs + obd::SPEED_MAX_AGE_MS + 1);
    selector.update(fixture.nowMs);
    selected = selector.selectedSpeed();
    TEST_ASSERT_FALSE(selected.valid);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, selected.source);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.runtime.snapshot(fixture.nowMs).staleSpeedCount);
}

void test_proxy_and_v1_contention_block_manual_scan_until_transport_is_available() {
    Fixture fixture;
    fixture.begin(true, "");
    TEST_ASSERT_TRUE(fixture.runtime.requestManualPairScan(fixture.nowMs));
    fixture.context.proxyAdvertising = true;
    fixture.context.v1ConnectInProgress = true;

    fixture.advance();
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, fixture.runtime.getState());
    TEST_ASSERT_EQUAL_UINT32(0, fixture.runtime.getStartScanCallCountForTest());

    fixture.context.proxyAdvertising = false;
    fixture.advance();
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, fixture.runtime.getState());
    TEST_ASSERT_EQUAL_UINT32(0, fixture.runtime.getStartScanCallCountForTest());

    fixture.context.v1ConnectInProgress = false;
    fixture.advance();
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, fixture.runtime.getState());
    TEST_ASSERT_EQUAL_UINT32(1, fixture.runtime.getStartScanCallCountForTest());
}

void test_shutdown_disable_and_restart_clear_session_before_reconnect() {
    Fixture fixture;
    fixture.bootToPolling();
    fixture.startSpeedCommand();
    fixture.sendResponse("41 0D 32\r>");
    TEST_ASSERT_TRUE(fixture.runtime.snapshot(fixture.nowMs).speedValid);

    TEST_ASSERT_TRUE(fixture.runtime.disconnectForShutdown(100));
    fixture.runtime.setEnabled(false);
    ObdRuntimeStatus status = fixture.runtime.snapshot(fixture.nowMs);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, status.state);
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FALSE(status.speedValid);

    fixture.runtime.setEnabled(true);
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, fixture.runtime.getState());
    fixture.advance();
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, fixture.runtime.getState());
}

void test_malformed_complete_response_is_rejected_without_speed_update() {
    Fixture fixture;
    fixture.bootToPolling();
    fixture.startSpeedCommand();
    fixture.sendResponse("41 0C GG\r>");

    const ObdRuntimeStatus status = fixture.runtime.snapshot(fixture.nowMs);
    TEST_ASSERT_EQUAL(ObdFailureReason::COMMAND_RESPONSE, status.lastFailure);
    TEST_ASSERT_EQUAL_UINT32(1, status.pollErrors);
    TEST_ASSERT_EQUAL_UINT32(0, status.pollCount);
    TEST_ASSERT_FALSE(status.speedValid);
}

void test_partial_response_completed_before_timeout_is_accepted() {
    Fixture fixture;
    fixture.bootToPolling();
    fixture.startSpeedCommand();
    fixture.sendResponse("41 0D", 100);
    TEST_ASSERT_EQUAL(ObdCommandKind::SPEED, fixture.runtime.snapshot(fixture.nowMs).commandInFlight);
    fixture.sendResponse(" 3C\r>", 100);

    const ObdRuntimeStatus status = fixture.runtime.snapshot(fixture.nowMs);
    TEST_ASSERT_TRUE(status.speedValid);
    TEST_ASSERT_EQUAL_UINT32(1, status.pollCount);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 37.2823f, status.speedMph);
}

void test_searching_response_extends_wait_and_accepts_delayed_elm_reply() {
    Fixture fixture;
    fixture.bootToPolling();
    const uint32_t issuedMs = fixture.startSpeedCommand();
    fixture.sendResponse("SEARCHING...\r", 100);

    fixture.update(issuedMs + obd::POLL_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(ObdCommandKind::SPEED, fixture.runtime.snapshot(fixture.nowMs).commandInFlight);
    fixture.runtime.onBleData(reinterpret_cast<const uint8_t*>("41 0D 32\r>"), 10);
    fixture.update(issuedMs + 9000);

    const ObdRuntimeStatus status = fixture.runtime.snapshot(fixture.nowMs);
    TEST_ASSERT_TRUE(status.speedValid);
    TEST_ASSERT_EQUAL_UINT32(1, status.pollCount);
    TEST_ASSERT_EQUAL_UINT32(0, status.pollErrors);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_initial_connection_runs_discovery_init_and_acquires_speed);
    RUN_TEST(test_manual_scan_times_out_to_idle_and_discards_candidate_session);
    RUN_TEST(test_connection_timeout_disconnects_and_enters_retry_state);
    RUN_TEST(test_discovery_transport_timeout_is_classified_as_discovery_failure);
    RUN_TEST(test_speed_command_transport_timeout_is_not_misreported_as_parse_failure);
    RUN_TEST(test_empty_response_retries_alternate_write_mode_then_times_out);
    RUN_TEST(test_partial_response_has_no_separate_parse_deadline_and_uses_response_timeout);
    RUN_TEST(test_disconnect_clears_speed_and_next_admitted_update_reconnects);
    RUN_TEST(test_old_session_response_queued_with_disconnect_cannot_seed_reconnect);
    RUN_TEST(test_real_speed_sample_is_primary_then_expires_and_invalidates_selection);
    RUN_TEST(test_proxy_and_v1_contention_block_manual_scan_until_transport_is_available);
    RUN_TEST(test_shutdown_disable_and_restart_clear_session_before_reconnect);
    RUN_TEST(test_malformed_complete_response_is_rejected_without_speed_update);
    RUN_TEST(test_partial_response_completed_before_timeout_is_accepted);
    RUN_TEST(test_searching_response_extends_wait_and_accepts_delayed_elm_reply);
    return UNITY_END();
}
