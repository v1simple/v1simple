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
#include "../../include/config.h"
#include "../mocks/ble_client_callback_stubs.h"

namespace {

int quiesceCalls = 0;

struct SubscriptionFixture {
    V1BLEClient client;
    NimBLEClient link;
    NimBLERemoteService service;
    NimBLERemoteCharacteristic display{V1_DISPLAY_DATA_UUID};
    NimBLERemoteCharacteristic command{V1_COMMAND_WRITE_UUID};
    NimBLERemoteCharacteristic displayLong{V1_DISPLAY_DATA_LONG_UUID};

    SubscriptionFixture() {
        client.pClient_ = &link;
        client.pRemoteService_ = &service;
        client.connectInProgress_ = true;
        client.connectStartMs_ = 10;
        client.shouldConnect_ = true;
        client.hasTargetDevice_ = true;
        client.bleState_ = BLEState::SUBSCRIBING;
    }

    void addMandatoryCharacteristics() {
        service.addCharacteristic(&display);
        service.addCharacteristic(&command);
    }
};

void assertImmediateFailure(SubscriptionFixture& fixture) {
    mockMillis = 100;
    fixture.client.processSubscribing();
    TEST_ASSERT_EQUAL_INT(1, quiesceCalls);
    TEST_ASSERT_EQUAL(BLEState::QUIESCING, fixture.client.bleState_);
    TEST_ASSERT_FALSE(fixture.client.connectInProgress_);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.client.connectStartMs_);
    TEST_ASSERT_FALSE(fixture.client.shouldConnect_.load(std::memory_order_relaxed));
    TEST_ASSERT_FALSE(fixture.client.hasTargetDevice_);
}

} // namespace

V1BLEClient::V1BLEClient() {}
V1BLEClient::~V1BLEClient() {}

void V1BLEClient::setBLEState(BLEState newState, const char*) {
    bleState_ = newState;
    stateEnteredMs_ = static_cast<uint32_t>(millis());
}

void V1BLEClient::beginClientQuiesce(const char*, bool) {
    quiesceCalls++;
    bleState_ = BLEState::QUIESCING;
}

void V1BLEClient::notifyCallback(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool) {}

#include "../../src/ble_subscription.cpp"

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
    quiesceCalls = 0;
}

void tearDown() {}

void test_missing_service_fails_without_deadline_retry() {
    SubscriptionFixture fixture;
    fixture.link.setService(nullptr);
    fixture.client.subscribeStep_ = V1BLEClient::SubscribeStep::GET_SERVICE;

    assertImmediateFailure(fixture);
}

void test_missing_display_characteristic_fails_without_deadline_retry() {
    SubscriptionFixture fixture;
    fixture.link.setService(&fixture.service);
    fixture.client.subscribeStep_ = V1BLEClient::SubscribeStep::GET_DISPLAY_CHAR;

    assertImmediateFailure(fixture);
}

void test_missing_command_characteristic_fails_without_deadline_retry() {
    SubscriptionFixture fixture;
    fixture.service.addCharacteristic(&fixture.display);
    fixture.link.setService(&fixture.service);
    fixture.client.subscribeStep_ = V1BLEClient::SubscribeStep::GET_COMMAND_CHAR;

    assertImmediateFailure(fixture);
}

void test_mandatory_display_subscription_failure_stops_immediately() {
    SubscriptionFixture fixture;
    fixture.addMandatoryCharacteristics();
    fixture.link.setService(&fixture.service);
    fixture.display.setSubscribeResult(false);
    fixture.client.pDisplayDataChar_ = &fixture.display;
    fixture.client.subscribeStep_ = V1BLEClient::SubscribeStep::SUBSCRIBE_DISPLAY;

    assertImmediateFailure(fixture);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.display.subscribeCalls());
}

void test_successful_steps_report_in_progress_then_complete_explicitly() {
    SubscriptionFixture fixture;
    fixture.addMandatoryCharacteristics();
    fixture.link.setService(&fixture.service);

    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStepResult::InProgress, fixture.client.executeSubscribeStep());
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStep::GET_DISPLAY_CHAR, fixture.client.subscribeStep_);
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStepResult::InProgress, fixture.client.executeSubscribeStep());
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStep::GET_COMMAND_CHAR, fixture.client.subscribeStep_);
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStepResult::InProgress, fixture.client.executeSubscribeStep());
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStep::GET_COMMAND_LONG, fixture.client.subscribeStep_);
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStepResult::InProgress, fixture.client.executeSubscribeStep());
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStep::SUBSCRIBE_DISPLAY, fixture.client.subscribeStep_);
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStepResult::InProgress, fixture.client.executeSubscribeStep());

    fixture.client.subscribeStep_ = V1BLEClient::SubscribeStep::REQUEST_VERSION;
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStepResult::Complete, fixture.client.executeSubscribeStep());
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStep::COMPLETE, fixture.client.subscribeStep_);
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStepResult::Complete, fixture.client.executeSubscribeStep());
}

void test_optional_long_subscription_failure_continues() {
    SubscriptionFixture fixture;
    fixture.addMandatoryCharacteristics();
    fixture.service.addCharacteristic(&fixture.displayLong);
    fixture.displayLong.setSubscribeResult(false);
    fixture.client.subscribeStep_ = V1BLEClient::SubscribeStep::SUBSCRIBE_LONG;

    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStepResult::InProgress, fixture.client.executeSubscribeStep());
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStep::REQUEST_ALERT_DATA, fixture.client.subscribeStep_);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.displayLong.subscribeCalls());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_missing_service_fails_without_deadline_retry);
    RUN_TEST(test_missing_display_characteristic_fails_without_deadline_retry);
    RUN_TEST(test_missing_command_characteristic_fails_without_deadline_retry);
    RUN_TEST(test_mandatory_display_subscription_failure_stops_immediately);
    RUN_TEST(test_successful_steps_report_in_progress_then_complete_explicitly);
    RUN_TEST(test_optional_long_subscription_failure_continues);
    return UNITY_END();
}
