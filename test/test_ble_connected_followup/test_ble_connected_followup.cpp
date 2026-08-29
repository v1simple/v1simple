#include <unity.h>

#include <deque>
#include <vector>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#define private public
#include "../../src/ble_client.h"
#undef private
#include "../mocks/ble_client_callback_stubs.h"

namespace {

struct SendAttempt {
    std::vector<uint8_t> bytes;
    SendResult result;
};

std::deque<SendResult> gResults;
std::vector<SendAttempt> gAttempts;
std::vector<std::vector<uint8_t>> gSentPackets;
int gStableCallbackCalls = 0;
int gAlertRequestCalls = 0;
std::deque<bool> gAlertRequestResults;

constexpr uint8_t kVersionRequest[] = {0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB};
constexpr uint8_t kAllVolumeRequest[] = {0xAA, 0xDA, 0xE6, 0x3C, 0x01, 0xA7, 0xAB};

void stableCallback() {
    ++gStableCallbackCalls;
}

void primeVersionRequest(V1BLEClient& client, uint32_t nowMs) {
    mockMillis = nowMs;
    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::REQUEST_VERSION;
    client.connectedFollowupNextAttemptMs_ = 0;
    client.connectedFollowupSendDeadlineMs_ = nowMs + V1BLEClient::CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
    client.versionRequestStartedMs_ = 0;
    client.v1FirmwareVersion_.store(0, std::memory_order_release);
}

void assertPacket(const uint8_t* expected, size_t expectedSize, const std::vector<uint8_t>& actual) {
    TEST_ASSERT_EQUAL_UINT(expectedSize, actual.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual.data(), expectedSize);
}

} // namespace

V1BLEClient::V1BLEClient() {}
V1BLEClient::~V1BLEClient() {}

bool V1BLEClient::requestAlertData() {
    ++gAlertRequestCalls;
    const bool result = gAlertRequestResults.empty() ? true : gAlertRequestResults.front();
    if (!gAlertRequestResults.empty()) {
        gAlertRequestResults.pop_front();
    }
    return result;
}

SendResult V1BLEClient::sendCommandWithResult(const uint8_t* data, size_t length) {
    const SendResult result = gResults.empty() ? SendResult::SENT : gResults.front();
    if (!gResults.empty()) {
        gResults.pop_front();
    }
    std::vector<uint8_t> bytes(data, data + length);
    gAttempts.push_back({bytes, result});
    if (result == SendResult::SENT) {
        gSentPackets.push_back(bytes);
    }
    return result;
}

int V1BLEClient::enqueueCurrentBondBackupSnapshot() {
    return 0;
}

#include "../../src/ble_connected_followup.cpp"

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
    gResults.clear();
    gAttempts.clear();
    gSentPackets.clear();
    gStableCallbackCalls = 0;
    gAlertRequestCalls = 0;
    gAlertRequestResults.clear();
    mock_reset_nimble_state();
}

void tearDown() {}

void test_alert_request_transient_failure_retries_then_settles() {
    V1BLEClient client;
    mockMillis = 100;
    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::REQUEST_ALERT_DATA;
    client.connectedFollowupSendDeadlineMs_ = mockMillis + V1BLEClient::CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
    gAlertRequestResults = {false, true};

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(1, gAlertRequestCalls);
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_ALERT_DATA,
                      client.connectedFollowupStep_);

    mockMillis = 104;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(1, gAlertRequestCalls);

    mockMillis = 105;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(2, gAlertRequestCalls);
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE,
                      client.connectedFollowupStep_);

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(2, gAlertRequestCalls);
}

void test_alert_request_retry_deadline_is_bounded() {
    V1BLEClient client;
    mockMillis = 200;
    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::REQUEST_ALERT_DATA;
    client.connectedFollowupSendDeadlineMs_ = 210;
    gAlertRequestResults = {false, false, false};

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(1, gAlertRequestCalls);

    mockMillis = 204;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(1, gAlertRequestCalls);

    mockMillis = 205;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(2, gAlertRequestCalls);
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_ALERT_DATA,
                      client.connectedFollowupStep_);

    mockMillis = 210;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(3, gAlertRequestCalls);
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE,
                      client.connectedFollowupStep_);
}

// V1-CONNECT-READBACK-001: transient deferrals retain the current request;
// successful writes occur exactly once in version -> all-volume order.
void test_not_yet_retries_in_order_without_spinning_or_duplicate_success() {
    V1BLEClient client;
    primeVersionRequest(client, 100);
    gResults = {SendResult::NOT_YET, SendResult::SENT, SendResult::NOT_YET, SendResult::SENT};

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_UINT(1, gAttempts.size());
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_VERSION, client.connectedFollowupStep_);
    assertPacket(kVersionRequest, sizeof(kVersionRequest), gAttempts[0].bytes);

    for (mockMillis = 100; mockMillis < 105; ++mockMillis) {
        client.processConnectedFollowup();
    }
    TEST_ASSERT_EQUAL_UINT(1, gAttempts.size());

    mockMillis = 105;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_ALL_VOLUME, client.connectedFollowupStep_);

    mockMillis = 109;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_UINT(2, gAttempts.size());

    mockMillis = 110;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_UINT(3, gAttempts.size());
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_ALL_VOLUME, client.connectedFollowupStep_);
    assertPacket(kAllVolumeRequest, sizeof(kAllVolumeRequest), gAttempts[2].bytes);

    for (mockMillis = 110; mockMillis < 115; ++mockMillis) {
        client.processConnectedFollowup();
    }
    TEST_ASSERT_EQUAL_UINT(3, gAttempts.size());

    mockMillis = 115;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_VERSION, client.connectedFollowupStep_);
    TEST_ASSERT_EQUAL_UINT(4, gAttempts.size());
    TEST_ASSERT_EQUAL_UINT(2, gSentPackets.size());
    assertPacket(kVersionRequest, sizeof(kVersionRequest), gSentPackets[0]);
    assertPacket(kAllVolumeRequest, sizeof(kAllVolumeRequest), gSentPackets[1]);

    for (mockMillis = 116; mockMillis < 130; ++mockMillis) {
        client.processConnectedFollowup();
    }
    TEST_ASSERT_EQUAL_UINT(4, gAttempts.size());
}

void test_version_terminal_failure_skips_volume_and_reaches_stable_callback() {
    V1BLEClient client;
    primeVersionRequest(client, 200);
    client.connectStableCallback_ = stableCallback;
    gResults = {SendResult::FAILED};

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_UINT(1, gAttempts.size());
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK, client.connectedFollowupStep_);
    assertPacket(kVersionRequest, sizeof(kVersionRequest), gAttempts[0].bytes);

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(1, gStableCallbackCalls);
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::BACKUP_BONDS, client.connectedFollowupStep_);
    TEST_ASSERT_EQUAL_UINT(1, gAttempts.size());
}

void test_all_volume_terminal_failure_does_not_resend_version_or_block_stable_callback() {
    V1BLEClient client;
    primeVersionRequest(client, 300);
    client.connectStableCallback_ = stableCallback;
    gResults = {SendResult::SENT, SendResult::FAILED};

    client.processConnectedFollowup();
    mockMillis = 305;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_VERSION, client.connectedFollowupStep_);
    TEST_ASSERT_EQUAL_UINT(2, gAttempts.size());
    TEST_ASSERT_EQUAL_UINT(1, gSentPackets.size());
    assertPacket(kVersionRequest, sizeof(kVersionRequest), gSentPackets[0]);
    assertPacket(kAllVolumeRequest, sizeof(kAllVolumeRequest), gAttempts[1].bytes);

    client.v1FirmwareVersion_.store(41038, std::memory_order_release);
    client.processConnectedFollowup();
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(1, gStableCallbackCalls);
    TEST_ASSERT_EQUAL_UINT(2, gAttempts.size());
}

void test_not_yet_deadline_is_terminal_and_does_not_busy_loop() {
    V1BLEClient client;
    primeVersionRequest(client, 400);
    client.connectedFollowupSendDeadlineMs_ = 410;
    gResults = {SendResult::NOT_YET, SendResult::NOT_YET, SendResult::NOT_YET};

    client.processConnectedFollowup();
    for (mockMillis = 400; mockMillis < 405; ++mockMillis) {
        client.processConnectedFollowup();
    }
    TEST_ASSERT_EQUAL_UINT(1, gAttempts.size());

    mockMillis = 405;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_UINT(2, gAttempts.size());
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_VERSION, client.connectedFollowupStep_);

    for (mockMillis = 405; mockMillis < 410; ++mockMillis) {
        client.processConnectedFollowup();
    }
    TEST_ASSERT_EQUAL_UINT(2, gAttempts.size());

    mockMillis = 410;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_UINT(3, gAttempts.size());
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK, client.connectedFollowupStep_);
    TEST_ASSERT_EQUAL_UINT(0, gSentPackets.size());
}

void test_all_volume_not_yet_deadline_continues_version_completion_path() {
    V1BLEClient client;
    primeVersionRequest(client, 450);
    client.connectStableCallback_ = stableCallback;
    gResults = {SendResult::SENT, SendResult::NOT_YET, SendResult::NOT_YET};

    client.processConnectedFollowup();
    client.connectedFollowupSendDeadlineMs_ = 460;
    mockMillis = 455;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_ALL_VOLUME, client.connectedFollowupStep_);

    for (mockMillis = 455; mockMillis < 460; ++mockMillis) {
        client.processConnectedFollowup();
    }
    TEST_ASSERT_EQUAL_UINT(2, gAttempts.size());

    mockMillis = 460;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_VERSION, client.connectedFollowupStep_);
    TEST_ASSERT_EQUAL_UINT(3, gAttempts.size());
    TEST_ASSERT_EQUAL_UINT(1, gSentPackets.size());
    assertPacket(kVersionRequest, sizeof(kVersionRequest), gSentPackets[0]);
    assertPacket(kAllVolumeRequest, sizeof(kAllVolumeRequest), gAttempts[1].bytes);
    assertPacket(kAllVolumeRequest, sizeof(kAllVolumeRequest), gAttempts[2].bytes);

    client.v1FirmwareVersion_.store(41038, std::memory_order_release);
    client.processConnectedFollowup();
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(1, gStableCallbackCalls);
    TEST_ASSERT_EQUAL_UINT(3, gAttempts.size());
}

void test_disconnect_none_cancels_retry_and_new_settle_restarts_at_version() {
    V1BLEClient client;
    primeVersionRequest(client, 500);
    gResults = {SendResult::NOT_YET, SendResult::SENT, SendResult::SENT};

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_UINT(1, gAttempts.size());

    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::NONE;
    client.connectedFollowupNextAttemptMs_ = 900;
    client.connectedFollowupSendDeadlineMs_ = 1200;
    mockMillis = 600;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_UINT(1, gAttempts.size());

    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE;
    client.connectCompletedAtMs_.store(590, std::memory_order_relaxed);
    client.firstRxAfterConnectMs_.store(0, std::memory_order_relaxed);
    client.connectBurstStableLoopCount_ = V1BLEClient::CONNECT_BURST_STABLE_CONSECUTIVE_LOOPS - 1;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_VERSION, client.connectedFollowupStep_);
    TEST_ASSERT_EQUAL_UINT32(0, client.connectedFollowupNextAttemptMs_);

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_ALL_VOLUME, client.connectedFollowupStep_);
    mockMillis += V1BLEClient::CONNECTED_FOLLOWUP_RETRY_MS;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_VERSION, client.connectedFollowupStep_);

    TEST_ASSERT_EQUAL_UINT(3, gAttempts.size());
    TEST_ASSERT_EQUAL_UINT(2, gSentPackets.size());
    assertPacket(kVersionRequest, sizeof(kVersionRequest), gSentPackets[0]);
    assertPacket(kAllVolumeRequest, sizeof(kAllVolumeRequest), gSentPackets[1]);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_alert_request_transient_failure_retries_then_settles);
    RUN_TEST(test_alert_request_retry_deadline_is_bounded);
    RUN_TEST(test_not_yet_retries_in_order_without_spinning_or_duplicate_success);
    RUN_TEST(test_version_terminal_failure_skips_volume_and_reaches_stable_callback);
    RUN_TEST(test_all_volume_terminal_failure_does_not_resend_version_or_block_stable_callback);
    RUN_TEST(test_not_yet_deadline_is_terminal_and_does_not_busy_loop);
    RUN_TEST(test_all_volume_not_yet_deadline_continues_version_completion_path);
    RUN_TEST(test_disconnect_none_cancels_retry_and_new_settle_restarts_at_version);
    return UNITY_END();
}
