#include <unity.h>

#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

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
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"
#include "../mocks/modules/power/power_module.h"
#include "../mocks/v1_profiles.h"
class DisplayPreviewModule { public: bool isRunning() const { return false; } void cancel() {} };
#include "../../src/modules/ble/ble_queue_module.cpp"

namespace {

std::function<void(const uint8_t*, size_t, uint16_t, uint32_t, uint32_t, uint32_t)> notificationSink;

void captureNotification(const uint8_t* bytes, size_t length, uint16_t characteristic, uint32_t generation,
                         uint32_t now, uint32_t sequence) {
    if (notificationSink) notificationSink(bytes, length, characteristic, generation, now, sequence);
}

void completePendingTask() {
    const auto function = g_mock_task_create_state.lastTaskFunction;
    void* parameter = g_mock_task_create_state.lastTaskParameter;
    g_mock_task_create_state.lastTaskFunction = nullptr;
    if (function) function(parameter);
}

struct SubscriptionFixture {
    V1BLEClient client;
    NimBLEClient link;
    NimBLERemoteService service{V1_SERVICE_UUID};
    V1BLEClient::ClientCallbacks callbacks;
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
        client.activeDiscoveryGeneration_ = 1;
        client.sessionGeneration_.store(1);
        client.acceptClientCallbacks_.store(true);
        client.sessionPublicationGate_.open(1);
        client.dataCallback_ = captureNotification;
        link.setConnected(true);
        link.setClientCallbacks(&callbacks);
        link.setDisconnectEntryHook([this]() { link.emitDisconnect(0); });
        client.activeConnectionHandle_.store(link.getConnHandle());
    }

    void addMandatoryCharacteristics() {
        service.addCharacteristic(&display);
        service.addCharacteristic(&command);
    }
};

void assertAttemptFailure(SubscriptionFixture& fixture) {
    mockMillis = 100;
    fixture.client.processSubscribing();
    if (fixture.client.discoveryTaskRunning_.load()) {
        completePendingTask();
        fixture.client.processSubscribing();
    }
    TEST_ASSERT_EQUAL(BLEState::QUIESCING, fixture.client.bleState_);
    TEST_ASSERT_FALSE(fixture.client.connectInProgress_);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.client.connectStartMs_);
    TEST_ASSERT_FALSE(fixture.client.shouldConnect_.load(std::memory_order_relaxed));
}

} // namespace

portMUX_TYPE pendingAddrMux = portMUX_INITIALIZER_UNLOCKED;
V1BLEClient* instancePtr = nullptr;

V1BLEClient::V1BLEClient()
    : pClient_(nullptr)
    , pRemoteService_(nullptr)
    , pDisplayDataChar_(nullptr)
    , pCommandChar_(nullptr)
    , pCommandCharLong_(nullptr)
    , pServer_(nullptr)
    , pProxyService_(nullptr)
    , pProxyNotifyChar_(nullptr)
    , pProxyNotifyLongChar_(nullptr)
    , pProxyWriteChar_(nullptr)
    , proxyEnabled_(false)
    , proxyServerInitialized_(false)
    , proxyServerInitAttempted_(false)
    , proxyName_("V1-Proxy")
    , proxyQueue_(nullptr)
    , phone2v1Queue_(nullptr)
    , proxyQueuesInPsram_(false)
    , dataCallback_(nullptr)
    , connectImmediateCallback_(nullptr)
    , sessionOpenedCallback_(nullptr)
    , sessionClosedCallback_(nullptr)
    , connectStableCallback_(nullptr)
    , targetAddress_()
    , lastScanStart_(0)
    , pScanCallbacks_(nullptr)
    , pClientCallbacks_(nullptr)
    , pProxyServerCallbacks_(nullptr)
    , pProxyWriteCallbacks_(nullptr) {
    instancePtr = this;
}

V1BLEClient::~V1BLEClient() {
    releaseProxyQueues();
}

void V1BLEClient::setBLEState(BLEState newState) {
    bleState_ = newState;
    stateEnteredMs_ = static_cast<uint32_t>(millis());
    scanStopResultsCleared_ = false;
}

void V1BLEClient::disconnect() {
    connected_.store(false, std::memory_order_relaxed);
}

void V1BLEClient::hardResetBLEClient() {
    connectInProgress_ = false;
    connectStartMs_ = 0;
    setBLEState(BLEState::DISCONNECTED);
}

void V1BLEClient::completeHardResetBLEClient() {}

void V1BLEClient::setProxyClientConnected(bool connectedState) {
    proxyClientConnected_ = connectedState;
    if (connectedState) {
        proxyClientConnectedOnceThisBoot_ = true;
        proxyFastAdvertisingUntilMs_ = 0;
    }
}

SendResult V1BLEClient::sendCommandWithResult(const uint8_t*, size_t) { return SendResult::SENT; }

SendResult V1BLEClient::sendCharacteristicCommand(NimBLERemoteCharacteristic*, const uint8_t* data, size_t length, bool, uint32_t) { return sendCommandWithResult(data, length); }
void V1BLEClient::cancelCommand(V1CommandOwner) {}

#include "../../src/ble_proxy.cpp"
#include "../../src/ble_connection.cpp"

void V1BLEClient::onUserBytesReceived(const uint8_t*, uint32_t) {}
void V1BLEClient::onV1FirmwareVersionReceived(uint32_t) {}

#ifndef BLE_SUBSCRIPTION_SOURCE
#define BLE_SUBSCRIPTION_SOURCE "../../src/ble_subscription.cpp"
#endif
#include BLE_SUBSCRIPTION_SOURCE

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
    mock_reset_task_create_state();
    mock_reset_task_delete_state();
    mock_reset_nimble_state();
    mock_reset_semaphore_state();
    notificationSink = nullptr;
}

void tearDown() {}

void test_missing_service_fails_without_deadline_retry() {
    SubscriptionFixture fixture;
    fixture.link.setService(nullptr);
    fixture.client.subscribeStep_ = V1BLEClient::SubscribeStep::GET_SERVICE;

    assertAttemptFailure(fixture);
}

void test_missing_display_characteristic_fails_without_deadline_retry() {
    SubscriptionFixture fixture;
    fixture.link.setService(&fixture.service);
    fixture.client.subscribeStep_ = V1BLEClient::SubscribeStep::GET_DISPLAY_CHAR;

    assertAttemptFailure(fixture);
}

void test_missing_command_characteristic_fails_without_deadline_retry() {
    SubscriptionFixture fixture;
    fixture.service.addCharacteristic(&fixture.display);
    fixture.link.setService(&fixture.service);
    fixture.client.subscribeStep_ = V1BLEClient::SubscribeStep::GET_COMMAND_CHAR;

    assertAttemptFailure(fixture);
}

void test_mandatory_display_subscription_failure_stops_immediately() {
    SubscriptionFixture fixture;
    fixture.addMandatoryCharacteristics();
    fixture.link.setService(&fixture.service);
    fixture.display.setSubscribeResult(false);
    fixture.client.pDisplayDataChar_ = &fixture.display;
    fixture.client.subscribeStep_ = V1BLEClient::SubscribeStep::SUBSCRIBE_DISPLAY;

    assertAttemptFailure(fixture);
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

    completePendingTask();
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStepResult::InProgress, fixture.client.executeSubscribeStep());
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStep::GET_DISPLAY_LONG, fixture.client.subscribeStep_);
    TEST_ASSERT_TRUE(fixture.display.subscribeResponse());

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
    assertAttemptFailure(fixture);
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
    assertAttemptFailure(fixture);

    // Transport reconnect creates a fresh accepted session, then the actual
    // subscription owner repeats discovery and publishes the connected state.
    fixture.client.processClientQuiesce();
    TEST_ASSERT_EQUAL(BLEState::DISCONNECTED, fixture.client.bleState_);
    fixture.link.setConnected(true);
    fixture.client.activeConnectionHandle_.store(fixture.link.getConnHandle());
    fixture.displayLong.setSubscribeResult(true);
    fixture.client.subscribeStep_ = V1BLEClient::SubscribeStep::GET_SERVICE;
    fixture.client.connectInProgress_ = true;
    fixture.client.connectStartMs_ = 100;
    fixture.client.activeDiscoveryGeneration_ = 2;
    fixture.client.sessionGeneration_.store(2);
    fixture.client.acceptClientCallbacks_.store(true);
    fixture.client.sessionPublicationGate_.open(2);
    for (unsigned i = 0; i < 24 && !fixture.client.connected_.load(); ++i) {
        mockMillis += 10;
        fixture.client.processSubscribing();
        completePendingTask();
    }
    TEST_ASSERT_TRUE(fixture.client.connected_.load());
    TEST_ASSERT_EQUAL_UINT32(2, fixture.displayLong.subscribeCalls());
    TEST_ASSERT_TRUE(fixture.displayLong.subscribeResponse());
    TEST_ASSERT_EQUAL_UINT32(0, fixture.link.lookupCalls());
    TEST_ASSERT_EQUAL_UINT32(0, fixture.service.lookupCalls());

    PacketParser parser;
    BleQueueModule queue;
    TEST_ASSERT_TRUE(queue.begin(&fixture.client, &parser, nullptr, nullptr, nullptr));
    queue.openSession(2);
    fixture.client.beginSessionSweepSectionsCapture(fixture.client.noteV1NotificationIngress());
    notificationSink = [&](const uint8_t* bytes, size_t length, uint16_t characteristic, uint32_t generation,
                           uint32_t now, uint32_t sequence) {
        TEST_ASSERT_EQUAL_UINT16(0xB4E0, characteristic);
        queue.onNotify(bytes, length, characteristic, generation, now, sequence);
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

namespace {

// Execute the function captured by the FreeRTOS creation mock on a real host
// thread, pausing inside the transport call. The wait is bounded so a broken
// launch/inline call fails the test instead of hanging the test runner.
class HeldSubscription {
  public:
    explicit HeldSubscription(NimBLERemoteCharacteristic& characteristic) {
        characteristic.setSubscribeEntryHook([this]() {
            std::unique_lock<std::mutex> lock(mutex_);
            entered_ = true;
            condition_.notify_all();
            expired_ = !condition_.wait_for(lock, std::chrono::seconds(2), [this]() { return released_; });
        });
    }

    bool start() {
        const auto function = g_mock_task_create_state.lastTaskFunction;
        void* parameter = g_mock_task_create_state.lastTaskParameter;
        g_mock_task_create_state.lastTaskFunction = nullptr;
        if (!function) return false;
        worker_ = std::thread([function, parameter]() { function(parameter); });
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [this]() { return entered_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    bool expired() const { return expired_; } // Read only after join.
    ~HeldSubscription() { release(); }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
    bool expired_ = false;
    std::thread worker_;
};

void prepareWrite(SubscriptionFixture& fixture, bool longChannel) {
    fixture.addMandatoryCharacteristics();
    fixture.service.addCharacteristic(&fixture.displayLong);
    fixture.link.setService(&fixture.service);
    fixture.client.pDisplayDataChar_ = &fixture.display;
    fixture.client.subscribeStep_ = longChannel ? V1BLEClient::SubscribeStep::SUBSCRIBE_LONG
                                               : V1BLEClient::SubscribeStep::SUBSCRIBE_DISPLAY;
    mockMillis = 100;
}

} // namespace

void test_delayed_ack_returns_to_loop_for_both_channels() {
    for (bool longChannel : {false, true}) {
        SubscriptionFixture fixture;
        prepareWrite(fixture, longChannel);
        const auto originalStep = fixture.client.subscribeStep_;
        auto& characteristic = longChannel ? fixture.displayLong : fixture.display;
        HeldSubscription held(characteristic);
        fixture.client.processSubscribing();
        const bool started = held.start();
        bool remainedPending = true;
        unsigned servicedLoopPasses = 0;
        for (unsigned i = 0; i < 8; ++i) {
            mockMillis += 10;
            fixture.client.processSubscribeYield();
            fixture.client.processSubscribing();
            // Work after BLE dispatch is still reachable while the real task
            // is inside subscribe(). No second worker may be launched.
            ++servicedLoopPasses;
            remainedPending &= fixture.client.discoveryTaskRunning_.load() &&
                               fixture.client.subscribeStep_ == originalStep && !fixture.client.connected_.load();
        }
        held.release(); // Always join before Unity can abort this test.
        TEST_ASSERT_TRUE(started);
        TEST_ASSERT_FALSE(held.expired());
        TEST_ASSERT_TRUE(remainedPending);
        TEST_ASSERT_EQUAL_UINT32(8, servicedLoopPasses);
        TEST_ASSERT_EQUAL_UINT32(1, characteristic.subscribeCalls());
        TEST_ASSERT_TRUE(characteristic.subscribeResponse());
        TEST_ASSERT_EQUAL(originalStep, fixture.client.subscribeStep_);
        fixture.client.processSubscribing();
        TEST_ASSERT_EQUAL(longChannel ? V1BLEClient::SubscribeStep::REQUEST_ALERT_DATA
                                     : V1BLEClient::SubscribeStep::GET_DISPLAY_LONG, fixture.client.subscribeStep_);
        TEST_ASSERT_EQUAL(BLEState::SUBSCRIBE_YIELD, fixture.client.bleState_);
    }
}

void test_subscribe_timeout_keeps_cache_alive_until_worker_exits() {
    SubscriptionFixture fixture;
    prepareWrite(fixture, true);
    HeldSubscription held(fixture.displayLong);
    fixture.client.processSubscribing();
    const bool started = held.start();
    mockMillis = 12000;
    fixture.client.processSubscribing();
    fixture.client.processClientQuiesce();
    const bool pending = fixture.client.bleState_ == BLEState::QUIESCING &&
                         fixture.client.pRemoteService_ == &fixture.service &&
                         fixture.client.pDisplayDataChar_ == &fixture.display &&
                         !fixture.client.connected_.load() && fixture.client.discoveryTaskRunning_.load();
    const bool reconnectRejected = !fixture.client.connectToServer();
    held.release();
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_FALSE(held.expired());
    TEST_ASSERT_TRUE(pending);
    TEST_ASSERT_TRUE(reconnectRejected);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.link.disconnectCalls());
    TEST_ASSERT_FALSE(fixture.client.sessionPublicationGate_.accepts(1));
    // A late successful ACK belongs to generation 1, already retired by the
    // actual timeout/quiesce path. Cleanup can now safely retire its handles.
    TEST_ASSERT_TRUE(fixture.client.discoveryTaskResult_.load());
    TEST_ASSERT_EQUAL_UINT32(1, fixture.client.discoveryCompletedGeneration_.load());
    fixture.client.processClientQuiesce();
    TEST_ASSERT_EQUAL(BLEState::DISCONNECTED, fixture.client.bleState_);
    TEST_ASSERT_NULL(fixture.client.pRemoteService_);
    TEST_ASSERT_NULL(fixture.client.pDisplayDataChar_);
    TEST_ASSERT_FALSE(fixture.client.discoveryTaskDone_.load());
    TEST_ASSERT_FALSE(fixture.client.connected_.load());
    TEST_ASSERT_TRUE(fixture.client.connectToServer());
}

void test_disconnect_after_ack_rejects_successful_completion() {
    SubscriptionFixture fixture;
    prepareWrite(fixture, true);
    fixture.client.processSubscribing();
    completePendingTask();
    TEST_ASSERT_TRUE(fixture.client.discoveryTaskResult_.load());
    fixture.link.emitDisconnect(0);
    fixture.client.processSubscribing();
    TEST_ASSERT_EQUAL(BLEState::QUIESCING, fixture.client.bleState_);
    TEST_ASSERT_FALSE(fixture.client.connected_.load());
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStep::SUBSCRIBE_LONG, fixture.client.subscribeStep_);
}

void test_worker_scheduled_after_timeout_does_not_touch_retired_session() {
    SubscriptionFixture fixture;
    prepareWrite(fixture, false);
    fixture.client.processSubscribing();
    mockMillis = 12000;
    fixture.client.processSubscribing();
    fixture.client.processClientQuiesce();
    TEST_ASSERT_EQUAL_PTR(&fixture.service, fixture.client.pRemoteService_);
    completePendingTask();
    TEST_ASSERT_EQUAL_UINT32(0, fixture.display.subscribeCalls());
    fixture.client.processClientQuiesce();
    TEST_ASSERT_NULL(fixture.client.pRemoteService_);
    TEST_ASSERT_FALSE(fixture.client.connected_.load());
}

void test_subscription_task_launch_failure_quiesces_without_inline_fallback() {
    SubscriptionFixture fixture;
    prepareWrite(fixture, false);
    g_mock_task_create_state.failCaps = true;
    fixture.client.processSubscribing();
    TEST_ASSERT_EQUAL(BLEState::QUIESCING, fixture.client.bleState_);
    TEST_ASSERT_FALSE(fixture.client.discoveryTaskRunning_.load());
    TEST_ASSERT_EQUAL_UINT32(0, fixture.display.subscribeCalls());
    fixture.client.processClientQuiesce();
    TEST_ASSERT_EQUAL(BLEState::DISCONNECTED, fixture.client.bleState_);
}

void test_indication_only_short_channel_keeps_acknowledged_subscription() {
    SubscriptionFixture fixture;
    prepareWrite(fixture, false);
    fixture.display.setCanNotify(false);
    fixture.display.setCanIndicate(true);
    fixture.client.processSubscribing();
    completePendingTask();
    fixture.client.processSubscribing();
    TEST_ASSERT_EQUAL_UINT32(1, fixture.display.subscribeCalls());
    TEST_ASSERT_FALSE(fixture.display.subscribeNotifications());
    TEST_ASSERT_TRUE(fixture.display.subscribeResponse());
    TEST_ASSERT_EQUAL(V1BLEClient::SubscribeStep::GET_DISPLAY_LONG, fixture.client.subscribeStep_);
}

void test_full_discovery_hands_worker_slot_to_subscription_and_uses_only_cache() {
    SubscriptionFixture fixture;
    fixture.addMandatoryCharacteristics();
    fixture.link.setService(&fixture.service);
    fixture.client.bleState_ = BLEState::DISCOVERING;
    mockMillis = 100;
    fixture.client.processDiscovering();
    completePendingTask();
    fixture.client.processDiscovering();
    TEST_ASSERT_EQUAL(BLEState::SUBSCRIBING, fixture.client.bleState_);
    TEST_ASSERT_FALSE(fixture.client.discoveryTaskDone_.load());
    for (unsigned i = 0; i < 24 && !fixture.client.connected_.load(); ++i) {
        fixture.client.processSubscribing();
        completePendingTask();
        mockMillis += 10;
    }
    TEST_ASSERT_TRUE(fixture.client.connected_.load());
    TEST_ASSERT_EQUAL_UINT32(1, fixture.display.subscribeCalls());
    TEST_ASSERT_EQUAL_UINT32(0, fixture.link.lookupCalls());
    TEST_ASSERT_EQUAL_UINT32(0, fixture.service.lookupCalls());
    TEST_ASSERT_EQUAL_UINT32(2, g_mock_task_create_state.capsCalls); // Discovery + short CCCD.
    TEST_ASSERT_EQUAL_UINT32(2, g_mock_task_delete_state.capsCalls);
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
    RUN_TEST(test_delayed_ack_returns_to_loop_for_both_channels);
    RUN_TEST(test_subscribe_timeout_keeps_cache_alive_until_worker_exits);
    RUN_TEST(test_disconnect_after_ack_rejects_successful_completion);
    RUN_TEST(test_worker_scheduled_after_timeout_does_not_touch_retired_session);
    RUN_TEST(test_subscription_task_launch_failure_quiesces_without_inline_fallback);
    RUN_TEST(test_indication_only_short_channel_keeps_acknowledged_subscription);
    RUN_TEST(test_full_discovery_hands_worker_slot_to_subscription_and_uses_only_cache);
    return UNITY_END();
}
