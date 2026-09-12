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
constexpr uint8_t kUserBytesRequest[] = {0xAA, 0xDA, 0xE6, 0x11, 0x01, 0x7C, 0xAB};

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
// successful writes occur exactly once in version -> all-volume -> user-bytes order.
void test_not_yet_retries_in_order_without_spinning_or_duplicate_success() {
    V1BLEClient client;
    primeVersionRequest(client, 100);
    gResults = {SendResult::NOT_YET, SendResult::SENT, SendResult::NOT_YET, SendResult::SENT,
                SendResult::SENT};

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
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_VERSION, client.connectedFollowupStep_);
    TEST_ASSERT_EQUAL_UINT(2, gAttempts.size());

    // No version-qualified requests are sent until RESP_VERSION arrives.
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_UINT(2, gAttempts.size());
    client.v1FirmwareVersion_.store(41037, std::memory_order_release);
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_ALL_VOLUME, client.connectedFollowupStep_);

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_UINT(3, gAttempts.size());
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_ALL_VOLUME, client.connectedFollowupStep_);
    assertPacket(kAllVolumeRequest, sizeof(kAllVolumeRequest), gAttempts[2].bytes);

    mockMillis = 110;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_USER_BYTES, client.connectedFollowupStep_);
    TEST_ASSERT_EQUAL_UINT(4, gAttempts.size());
    TEST_ASSERT_EQUAL_UINT(2, gSentPackets.size());
    assertPacket(kVersionRequest, sizeof(kVersionRequest), gSentPackets[0]);
    assertPacket(kAllVolumeRequest, sizeof(kAllVolumeRequest), gSentPackets[1]);

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_SETTINGS_SNAPSHOT, client.connectedFollowupStep_);
    TEST_ASSERT_EQUAL_UINT(5, gAttempts.size());
    TEST_ASSERT_EQUAL_UINT(3, gSentPackets.size());
    assertPacket(kUserBytesRequest, sizeof(kUserBytesRequest), gSentPackets[2]);
}

void test_version_terminal_failure_skips_volume_and_reaches_stable_callback() {
    V1BLEClient client;
    primeVersionRequest(client, 200);
    client.connectStableCallback_ = stableCallback;
    gResults = {SendResult::FAILED};

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_UINT(1, gAttempts.size());
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_USER_BYTES, client.connectedFollowupStep_);
    TEST_ASSERT_TRUE(client.settingsCaptureTimedOut_);
    assertPacket(kVersionRequest, sizeof(kVersionRequest), gAttempts[0].bytes);

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_SETTINGS_SNAPSHOT, client.connectedFollowupStep_);
    client.hasSessionUserBytes_ = true;
    client.processConnectedFollowup();
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(1, gStableCallbackCalls);
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::BACKUP_BONDS, client.connectedFollowupStep_);
    TEST_ASSERT_EQUAL_UINT(2, gAttempts.size());
}

void test_all_volume_terminal_failure_does_not_resend_version_or_block_stable_callback() {
    V1BLEClient client;
    primeVersionRequest(client, 300);
    client.connectStableCallback_ = stableCallback;
    gResults = {SendResult::SENT, SendResult::FAILED, SendResult::SENT};

    client.processConnectedFollowup(); // version request
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_VERSION, client.connectedFollowupStep_);
    client.v1FirmwareVersion_.store(41038, std::memory_order_release);
    client.processConnectedFollowup(); // version gate
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_ALL_VOLUME, client.connectedFollowupStep_);
    mockMillis = 305;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_USER_BYTES, client.connectedFollowupStep_);
    TEST_ASSERT_EQUAL_UINT(2, gAttempts.size());
    TEST_ASSERT_EQUAL_UINT(1, gSentPackets.size());
    assertPacket(kVersionRequest, sizeof(kVersionRequest), gSentPackets[0]);
    assertPacket(kAllVolumeRequest, sizeof(kAllVolumeRequest), gAttempts[1].bytes);
    TEST_ASSERT_TRUE(client.settingsCaptureTimedOut_);
    TEST_ASSERT_FALSE(client.expectsSessionAllVolume_);

    client.processConnectedFollowup();
    client.hasSessionUserBytes_ = true;
    client.processConnectedFollowup();
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(1, gStableCallbackCalls);
    TEST_ASSERT_EQUAL_UINT(3, gAttempts.size());
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
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_USER_BYTES, client.connectedFollowupStep_);
    TEST_ASSERT_EQUAL_UINT(0, gSentPackets.size());
}

void test_all_volume_not_yet_deadline_marks_partial_and_continues_to_user_bytes() {
    V1BLEClient client;
    primeVersionRequest(client, 450);
    client.connectStableCallback_ = stableCallback;
    gResults = {SendResult::SENT, SendResult::NOT_YET, SendResult::NOT_YET, SendResult::NOT_YET,
                SendResult::SENT};

    client.processConnectedFollowup(); // version request
    client.v1FirmwareVersion_.store(41038, std::memory_order_release);
    client.processConnectedFollowup(); // version gate
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_ALL_VOLUME, client.connectedFollowupStep_);
    client.connectedFollowupSendDeadlineMs_ = 460;

    client.processConnectedFollowup(); // first all-volume attempt
    mockMillis = 455;
    client.processConnectedFollowup(); // second attempt
    TEST_ASSERT_EQUAL_UINT(3, gAttempts.size());

    mockMillis = 460;
    client.processConnectedFollowup(); // terminal third attempt
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_USER_BYTES, client.connectedFollowupStep_);
    TEST_ASSERT_EQUAL_UINT(4, gAttempts.size());
    TEST_ASSERT_EQUAL_UINT(1, gSentPackets.size());
    assertPacket(kVersionRequest, sizeof(kVersionRequest), gSentPackets[0]);
    assertPacket(kAllVolumeRequest, sizeof(kAllVolumeRequest), gAttempts[1].bytes);
    assertPacket(kAllVolumeRequest, sizeof(kAllVolumeRequest), gAttempts[2].bytes);
    assertPacket(kAllVolumeRequest, sizeof(kAllVolumeRequest), gAttempts[3].bytes);
    TEST_ASSERT_TRUE(client.settingsCaptureTimedOut_);

    client.processConnectedFollowup();
    client.hasSessionUserBytes_ = true;
    client.processConnectedFollowup();
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(1, gStableCallbackCalls);
    TEST_ASSERT_EQUAL_UINT(5, gAttempts.size());
}

void test_disconnect_none_cancels_retry_and_new_settle_restarts_at_version() {
    V1BLEClient client;
    primeVersionRequest(client, 500);
    gResults = {SendResult::NOT_YET, SendResult::SENT, SendResult::SENT, SendResult::SENT};

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
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_VERSION, client.connectedFollowupStep_);
    client.v1FirmwareVersion_.store(41038, std::memory_order_release);
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_ALL_VOLUME, client.connectedFollowupStep_);
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_USER_BYTES, client.connectedFollowupStep_);
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_SETTINGS_SNAPSHOT, client.connectedFollowupStep_);

    TEST_ASSERT_EQUAL_UINT(4, gAttempts.size());
    TEST_ASSERT_EQUAL_UINT(3, gSentPackets.size());
    assertPacket(kVersionRequest, sizeof(kVersionRequest), gSentPackets[0]);
    assertPacket(kAllVolumeRequest, sizeof(kAllVolumeRequest), gSentPackets[1]);
    assertPacket(kUserBytesRequest, sizeof(kUserBytesRequest), gSentPackets[2]);
}

void test_stable_callback_waits_for_pre_apply_user_bytes() {
    V1BLEClient client;
    client.connectStableCallback_ = stableCallback;
    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::REQUEST_USER_BYTES;
    client.connectedFollowupSendDeadlineMs_ = 1600;
    client.v1FirmwareVersion_.store(41039, std::memory_order_release);
    client.expectsSessionAllVolume_ = true;

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_SETTINGS_SNAPSHOT, client.connectedFollowupStep_);
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(0, gStableCallbackCalls);

    client.hasSessionUserBytes_ = true;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_SETTINGS_SNAPSHOT, client.connectedFollowupStep_);
    client.hasSessionAllVolume_ = true;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK, client.connectedFollowupStep_);
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(1, gStableCallbackCalls);
    TEST_ASSERT_FALSE(client.settingsCaptureTimedOut_);
}

void test_pre_41037_version_skips_unsupported_all_volume_request() {
    V1BLEClient client;
    client.connectStableCallback_ = stableCallback;
    primeVersionRequest(client, 1700);
    gResults = {SendResult::SENT, SendResult::SENT};

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_VERSION, client.connectedFollowupStep_);
    client.v1FirmwareVersion_.store(41036, std::memory_order_release);
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_USER_BYTES, client.connectedFollowupStep_);
    client.processConnectedFollowup();
    client.hasSessionUserBytes_ = true;
    client.processConnectedFollowup();
    client.processConnectedFollowup();

    TEST_ASSERT_EQUAL_INT(1, gStableCallbackCalls);
    TEST_ASSERT_FALSE(client.settingsCaptureTimedOut_);
    TEST_ASSERT_EQUAL_UINT(2, gSentPackets.size());
    assertPacket(kVersionRequest, sizeof(kVersionRequest), gSentPackets[0]);
    assertPacket(kUserBytesRequest, sizeof(kUserBytesRequest), gSentPackets[1]);
}

void test_unknown_version_timeout_skips_all_volume_and_marks_partial() {
    V1BLEClient client;
    client.connectStableCallback_ = stableCallback;
    primeVersionRequest(client, 1800);
    gResults = {SendResult::SENT, SendResult::SENT};

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_VERSION, client.connectedFollowupStep_);
    mockMillis = 1800 + V1BLEClient::VERSION_RESPONSE_TIMEOUT_MS - 1;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_UINT(1, gAttempts.size());
    mockMillis = 1800 + V1BLEClient::VERSION_RESPONSE_TIMEOUT_MS;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::REQUEST_USER_BYTES, client.connectedFollowupStep_);
    TEST_ASSERT_TRUE(client.settingsCaptureTimedOut_);
    client.processConnectedFollowup();
    client.hasSessionUserBytes_ = true;
    client.processConnectedFollowup();
    client.processConnectedFollowup();

    TEST_ASSERT_EQUAL_INT(1, gStableCallbackCalls);
    TEST_ASSERT_EQUAL_UINT(2, gSentPackets.size());
    assertPacket(kUserBytesRequest, sizeof(kUserBytesRequest), gSentPackets[1]);
}

void test_missing_expected_all_volume_response_times_out_as_partial() {
    V1BLEClient client;
    client.connectStableCallback_ = stableCallback;
    primeVersionRequest(client, 2000);
    gResults = {SendResult::SENT, SendResult::SENT, SendResult::SENT};

    client.processConnectedFollowup();
    client.v1FirmwareVersion_.store(41037, std::memory_order_release);
    client.processConnectedFollowup();
    client.processConnectedFollowup();
    client.processConnectedFollowup();
    client.hasSessionUserBytes_ = true;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_SETTINGS_SNAPSHOT, client.connectedFollowupStep_);
    mockMillis = client.settingsCaptureRequestStartedMs_ + V1BLEClient::SETTINGS_SNAPSHOT_RESPONSE_TIMEOUT_MS;
    client.processConnectedFollowup();
    TEST_ASSERT_TRUE(client.settingsCaptureTimedOut_);
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK, client.connectedFollowupStep_);
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(1, gStableCallbackCalls);
}

void test_missing_snapshot_response_times_out_as_partial_before_callback() {
    V1BLEClient client;
    client.connectStableCallback_ = stableCallback;
    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::REQUEST_USER_BYTES;
    client.connectedFollowupSendDeadlineMs_ = 1600;

    client.processConnectedFollowup();
    mockMillis = V1BLEClient::SETTINGS_SNAPSHOT_RESPONSE_TIMEOUT_MS;
    client.processConnectedFollowup();
    TEST_ASSERT_TRUE(client.settingsCaptureTimedOut_);
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK, client.connectedFollowupStep_);
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(1, gStableCallbackCalls);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_alert_request_transient_failure_retries_then_settles);
    RUN_TEST(test_alert_request_retry_deadline_is_bounded);
    RUN_TEST(test_not_yet_retries_in_order_without_spinning_or_duplicate_success);
    RUN_TEST(test_version_terminal_failure_skips_volume_and_reaches_stable_callback);
    RUN_TEST(test_all_volume_terminal_failure_does_not_resend_version_or_block_stable_callback);
    RUN_TEST(test_not_yet_deadline_is_terminal_and_does_not_busy_loop);
    RUN_TEST(test_all_volume_not_yet_deadline_marks_partial_and_continues_to_user_bytes);
    RUN_TEST(test_disconnect_none_cancels_retry_and_new_settle_restarts_at_version);
    RUN_TEST(test_stable_callback_waits_for_pre_apply_user_bytes);
    RUN_TEST(test_pre_41037_version_skips_unsupported_all_volume_request);
    RUN_TEST(test_unknown_version_timeout_skips_all_volume_and_marks_partial);
    RUN_TEST(test_missing_expected_all_volume_response_times_out_as_partial);
    RUN_TEST(test_missing_snapshot_response_times_out_as_partial_before_callback);
    return UNITY_END();
}
