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
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"
#include "../mocks/modules/power/power_module.h"
#include "../mocks/v1_profiles.h"
class DisplayPreviewModule { public: bool isRunning() const { return false; } void cancel() {} };
#include "../../src/modules/ble/ble_queue_module.cpp"

namespace {

int quiesceCalls = 0;
std::function<void(NimBLERemoteCharacteristic*, uint8_t*, size_t)> notificationSink;

struct SubscriptionFixture {
    V1BLEClient client;
    NimBLEClient link;
    NimBLERemoteService service;
    NimBLERemoteCharacteristic display{V1_DISPLAY_DATA_UUID};
    NimBLERemoteCharacteristic command{V1_COMMAND_WRITE_UUID};
    NimBLERemoteCharacteristic displayLong{V1_DISPLAY_DATA_LONG_UUID};

    SubscriptionFixture() {
        client.connectImmediateCallback_ = nullptr;
        client.pClient_ = &link;
        client.pRemoteService_ = &service;
        client.connectInProgress_ = true;
        client.connectStartMs_ = 10;
        client.shouldConnect_ = true;
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
}

} // namespace

V1BLEClient::V1BLEClient() {}
V1BLEClient::~V1BLEClient() {}

void V1BLEClient::setBLEState(BLEState newState) {
    bleState_ = newState;
    stateEnteredMs_ = static_cast<uint32_t>(millis());
}

void V1BLEClient::beginClientQuiesce(bool) {
    quiesceCalls++;
    bleState_ = BLEState::QUIESCING;
}

void V1BLEClient::notifyCallback(NimBLERemoteCharacteristic* characteristic, uint8_t* bytes, size_t length, bool) {
    if (notificationSink) notificationSink(characteristic, bytes, length);
}
int V1BLEClient::processProxyQueue() { return 0; }
void V1BLEClient::onUserBytesReceived(const uint8_t*, uint32_t) {}
void V1BLEClient::onV1FirmwareVersionReceived(uint32_t) {}

#include "../../src/ble_subscription.cpp"

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
    quiesceCalls = 0;
    notificationSink = nullptr;
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


void test_present_long_subscription_failure_quiesces_instead_of_publishing_connected() {
    SubscriptionFixture fixture;
    fixture.addMandatoryCharacteristics();
    fixture.service.addCharacteristic(&fixture.displayLong);
    fixture.displayLong.setSubscribeResult(false);
    fixture.client.subscribeStep_ = V1BLEClient::SubscribeStep::SUBSCRIBE_LONG;
    assertImmediateFailure(fixture);
    TEST_ASSERT_FALSE(fixture.client.connected_.load());
    TEST_ASSERT_EQUAL_UINT32(1, fixture.displayLong.subscribeCalls());
}

void test_missing_or_non_notifiable_long_channel_remains_optional() {
    for (bool present : {false, true}) {
        SubscriptionFixture fixture;
        fixture.addMandatoryCharacteristics();
        if (present) fixture.service.addCharacteristic(&fixture.displayLong);
        fixture.displayLong.setCanNotify(false);
        fixture.client.subscribeStep_ = V1BLEClient::SubscribeStep::GET_DISPLAY_LONG;
        TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStepResult::InProgress, fixture.client.executeSubscribeStep());
        TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStep::REQUEST_ALERT_DATA, fixture.client.subscribeStep_);
        TEST_ASSERT_EQUAL_UINT32(0, fixture.displayLong.subscribeCalls());
    }
}

void test_reconnect_after_failed_long_subscription_delivers_complete_sweep_response() {
    SubscriptionFixture fixture;
    fixture.addMandatoryCharacteristics();
    fixture.service.addCharacteristic(&fixture.displayLong);
    fixture.link.setService(&fixture.service);
    fixture.displayLong.setSubscribeResult(false);
    fixture.client.subscribeStep_ = V1BLEClient::SubscribeStep::SUBSCRIBE_LONG;
    assertImmediateFailure(fixture);

    // Transport reconnect creates a fresh accepted session, then the actual
    // subscription owner repeats discovery and publishes the connected state.
    fixture.displayLong.setSubscribeResult(true);
    fixture.client.subscribeStep_ = V1BLEClient::SubscribeStep::GET_SERVICE;
    fixture.client.connectInProgress_ = true;
    fixture.client.connectStartMs_ = 100;
    fixture.client.activeDiscoveryGeneration_ = 2;
    fixture.client.sessionGeneration_.store(2);
    fixture.client.acceptClientCallbacks_.store(true);
    fixture.client.sessionPublicationGate_.open(2);
    for (unsigned i = 0; i < 10 && !fixture.client.connected_.load(); ++i) {
        mockMillis += 10;
        fixture.client.processSubscribing();
    }
    TEST_ASSERT_TRUE(fixture.client.connected_.load());
    TEST_ASSERT_EQUAL_UINT32(2, fixture.displayLong.subscribeCalls());

    PacketParser parser;
    BleQueueModule queue;
    TEST_ASSERT_TRUE(queue.begin(&fixture.client, &parser, nullptr, nullptr, nullptr));
    queue.openSession(2);
    fixture.client.beginSessionSweepSectionsCapture(fixture.client.noteV1NotificationIngress());
    notificationSink = [&](NimBLERemoteCharacteristic* characteristic, uint8_t* bytes, size_t length) {
        TEST_ASSERT_EQUAL_PTR(&fixture.displayLong, characteristic);
        queue.onNotify(bytes, length, 0xB4E0, 2, mockMillis, fixture.client.noteV1NotificationIngress());
    };
    // Three valid section entries plus checksum/end require B4E0.
    std::vector<uint8_t> frame{0xAA, 0xD6, 0xEA, 0x23, 16,
        0x13, 0x61, 0xA8, 0x5D, 0xC0,
        0x23, 0x00, 0x00, 0x00, 0x00,
        0x33, 0x8C, 0xA0, 0x80, 0xE8};
    uint8_t checksum = 0;
    for (uint8_t byte : frame) checksum = static_cast<uint8_t>(checksum + byte);
    frame.push_back(checksum);
    frame.push_back(0xAB);
    TEST_ASSERT_GREATER_THAN_UINT(20, frame.size());
    for (size_t index = 0; index < 2; ++index) {
        std::vector<uint8_t> chunk{static_cast<uint8_t>(((index + 1) << 4) | 2)};
        const size_t offset = index * 19;
        chunk.insert(chunk.end(), frame.begin() + offset, frame.begin() + std::min(offset + 19, frame.size()));
        fixture.displayLong.emit(chunk.data(), chunk.size());
        queue.process();
    }
    TEST_ASSERT_TRUE(parser.sweepSectionsObservation().complete);
    TEST_ASSERT_EQUAL_UINT8(3, parser.sweepSectionsObservation().count);
    TEST_ASSERT_EQUAL_UINT16(33000, parser.sweepSectionsObservation().sections[2].lowerMHz);
    TEST_ASSERT_TRUE(fixture.client.hasSessionSweepSectionsCapture());
    notificationSink = nullptr;
    queue.end();
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_missing_service_fails_without_deadline_retry);
    RUN_TEST(test_missing_display_characteristic_fails_without_deadline_retry);
    RUN_TEST(test_missing_command_characteristic_fails_without_deadline_retry);
    RUN_TEST(test_mandatory_display_subscription_failure_stops_immediately);
    RUN_TEST(test_successful_steps_report_in_progress_then_complete_explicitly);
    RUN_TEST(test_present_long_subscription_failure_quiesces_instead_of_publishing_connected);
    RUN_TEST(test_missing_or_non_notifiable_long_channel_remains_optional);
    RUN_TEST(test_reconnect_after_failed_long_subscription_delivers_complete_sweep_response);
    return UNITY_END();
}
