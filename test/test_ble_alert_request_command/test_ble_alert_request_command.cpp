#include <unity.h>

#include <cstdint>

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

V1BLEClient::V1BLEClient() {}
V1BLEClient::~V1BLEClient() {}
bool V1BLEClient::isConnected() {
    return connected_.load(std::memory_order_acquire) && pClient_ && pClient_->isConnected();
}

#include "../../src/ble_commands.cpp"

namespace {

struct AlertRequestHarness {
    V1BLEClient client;
    NimBLEClient link;
    NimBLERemoteCharacteristic command{"B6D4"};

    explicit AlertRequestHarness(uint32_t generation) {
        link.setConnected(true);
        client.connected_.store(true, std::memory_order_release);
        client.sessionGeneration_.store(generation, std::memory_order_release);
        client.pClient_ = &link;
        client.pCommandChar_ = &command;
    }
};

} // namespace

void setUp() {}
void tearDown() {}

void test_command_guard_uses_successful_send_time_for_exact_boundary() {
    AlertRequestHarness harness(7);

    mockMillis = 5050;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(1, harness.command.writeValueCalls());

    // The stale owner runs 1001 ms after connection, but only 951 ms after
    // the delayed initial transmission. It must not reach the transport.
    mockMillis = 6001;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(1, harness.command.writeValueCalls());

    mockMillis = 6049;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(1, harness.command.writeValueCalls());

    mockMillis = 6050;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(2, harness.command.writeValueCalls());

    mockMillis = 7049;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(2, harness.command.writeValueCalls());

    mockMillis = 7050;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(3, harness.command.writeValueCalls());
}

void test_failed_transport_write_does_not_consume_command_guard_slot() {
    AlertRequestHarness harness(7);
    harness.command.setWriteValueResult(false);

    mockMillis = 20000;
    TEST_ASSERT_FALSE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(1, harness.command.writeValueCalls());

    harness.command.setWriteValueResult(true);
    mockMillis = 20005;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(2, harness.command.writeValueCalls());

    mockMillis = 21004;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(2, harness.command.writeValueCalls());

    mockMillis = 21005;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(3, harness.command.writeValueCalls());
}

void test_new_session_generation_gets_an_immediate_first_command() {
    AlertRequestHarness harness(7);

    mockMillis = 30000;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(1, harness.command.writeValueCalls());

    harness.client.sessionGeneration_.store(9, std::memory_order_release);
    mockMillis = 30010;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(2, harness.command.writeValueCalls());

    mockMillis = 30020;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(2, harness.command.writeValueCalls());

    mockMillis = 31010;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(3, harness.command.writeValueCalls());
}

void test_command_guard_interval_survives_millis_wrap() {
    AlertRequestHarness harness(11);

    mockMillis = UINT32_MAX - 500;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(1, harness.command.writeValueCalls());

    mockMillis = 498;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(1, harness.command.writeValueCalls());

    mockMillis = 499;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(2, harness.command.writeValueCalls());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_command_guard_uses_successful_send_time_for_exact_boundary);
    RUN_TEST(test_failed_transport_write_does_not_consume_command_guard_slot);
    RUN_TEST(test_new_session_generation_gets_an_immediate_first_command);
    RUN_TEST(test_command_guard_interval_survives_millis_wrap);
    return UNITY_END();
}
