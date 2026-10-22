#include <unity.h>
#include <atomic>
#include "../mocks/Arduino.h"
#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif
#include "../mocks/freertos/FreeRTOS.h"
#include "../mocks/freertos/task.h"
#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/esp_heap_caps.h"
#ifndef configASSERT
#define configASSERT(expr) do { if (!(expr)) TEST_FAIL_MESSAGE("configASSERT failed"); } while (0)
#endif
#define private public
#include "../../src/ble_client.h"
#undef private

portMUX_TYPE pendingAddrMux = portMUX_INITIALIZER_UNLOCKED;

// Transport setup, connection observation, clock, locks and NimBLE are doubles.
// The phone callbacks, deferred consumer, flow control and final command send
// (including the newer local unmute) are the production owners.
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
}

V1BLEClient::~V1BLEClient() {
    releaseProxyQueues();
}

void V1BLEClient::setProxyClientConnected(bool connectedState) {
    proxyClientConnected_ = connectedState;
    if (connectedState) {
        proxyClientConnectedOnceThisBoot_ = true;
        proxyFastAdvertisingUntilMs_ = 0;
    }
}

bool V1BLEClient::isConnected() {
    return connected_.load(std::memory_order_acquire) && pClient_ && pClient_->isConnected();
}

#include "../../src/ble_proxy.cpp"
void V1BLEClient::beginClientQuiesce(bool) { connected_.store(false); bleState_ = BLEState::QUIESCING; sessionPublicationGate_.close(); }

#include "../../src/ble_commands.cpp"

void setUp() {
    mock_reset_heap_caps();
    mock_reset_semaphore_state();
    mock_reset_nimble_state();
    static uint32_t testStartMs = 1000;
    testStartMs += 10000;
    mockMillis = testStartMs;
    mockMicros = 0;
}
void tearDown() {}

namespace {
const uint8_t muteOn[] = {0xAA, 0xDA, 0xE6, 0x34, 0x01, 0x9F, 0xAB};
const uint8_t muteOff[] = {0xAA, 0xDA, 0xE6, 0x35, 0x01, 0xA0, 0xAB};

struct PhoneFixture {
    V1BLEClient client;
    NimBLEClient link;
    NimBLEConnInfo phone;
    NimBLECharacteristic input{V1_COMMAND_WRITE_UUID};
    NimBLERemoteCharacteristic remote{V1_COMMAND_WRITE_UUID};
    V1BLEClient::ProxyServerCallbacks lifecycle{&client};
    V1BLEClient::ProxyWriteCallbacks writer{&client};

    PhoneFixture() {
        TEST_ASSERT_TRUE(client.allocateProxyQueues());
        client.phoneCmdMutex_ = xSemaphoreCreateMutex();
        client.bleMutex_ = xSemaphoreCreateMutex();
        link.setConnected(true);
        client.pClient_ = &link;
        client.pCommandChar_ = &remote;
        client.connected_.store(true, std::memory_order_release);
        client.sessionGeneration_.store(1, std::memory_order_release);
        client.sessionPublicationGate_.open(1);
        client.onV1DisplayFlowControl(false);
        lifecycle.onConnect(nullptr, phone);
        client.drainProxyCallbackEvents();
    }
    template <size_t N>
    void write(const uint8_t (&packet)[N]) {
        input.setValue(packet, N);
        writer.onWrite(&input, phone);
    }
    void holdMute() {
        const uint8_t request[] = {PACKET_ID_MUTE_ON};
        client.onV1Busy(request, sizeof(request));
    }
    void releaseHold() {
        client.onV1DisplayFlowControl(false);
        client.onV1DisplayFlowControl(false);
        mockMillis += 100;
    }
    void reconnect() {
        lifecycle.onDisconnect(nullptr, phone, 0);
        lifecycle.onConnect(nullptr, phone);
    }
};
}

void test_retired_queued_or_pending_phone_mute_cannot_override_new_local_unmute() {
    for (bool pending : {false, true}) {
        for (bool notifyLock : {false, true}) {
            PhoneFixture f;
            if (notifyLock) f.client.bleNotifyMutex_ = xSemaphoreCreateMutex();
            f.holdMute();
            f.write(muteOn);
            if (pending) TEST_ASSERT_EQUAL_INT(0, f.client.processPhoneCommandQueue());
            TEST_ASSERT_EQUAL_UINT32(0, f.remote.writeValueCalls());
            f.lifecycle.onDisconnect(nullptr, f.phone, 0);
            f.client.drainProxyCallbackEvents();
            mockMillis += 100;
            TEST_ASSERT_EQUAL(SendResult::SENT, f.client.setMuteResult(false));
            f.releaseHold();
            TEST_ASSERT_EQUAL_INT(0, f.client.processPhoneCommandQueue());
            TEST_ASSERT_EQUAL_UINT32(1, f.remote.writeValueCalls());
            TEST_ASSERT_EQUAL_UINT8_ARRAY(muteOff, f.remote.lastWriteValue().data(), sizeof(muteOff));
        }
    }
}

void test_same_phone_deferral_preserves_mute_and_rejects_local_unmute() {
    PhoneFixture f;
    f.holdMute();
    f.write(muteOn);
    TEST_ASSERT_EQUAL_INT(0, f.client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL(SendResult::FAILED, f.client.setMuteResult(false));
    f.releaseHold();
    TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL_UINT32(1, f.remote.writeValueCalls());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(muteOn, f.remote.lastWriteValue().data(), sizeof(muteOn));
}

void test_quick_phone_reconnect_keeps_new_work_and_discards_retired_pending_and_queue() {
    PhoneFixture f;
    const uint32_t v1Generation = f.client.sessionGeneration();
    f.holdMute();
    f.write(muteOn);
    TEST_ASSERT_EQUAL_INT(0, f.client.processPhoneCommandQueue());
    f.write(muteOn);
    f.reconnect();
    f.write(muteOff);
    f.client.drainProxyCallbackEvents();
    TEST_ASSERT_TRUE(f.client.proxyClientConnected_.load());
    TEST_ASSERT_EQUAL_UINT32(v1Generation, f.client.sessionGeneration());
    f.releaseHold();
    TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL_INT(0, f.client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL_UINT32(1, f.remote.writeValueCalls());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(muteOff, f.remote.lastWriteValue().data(), sizeof(muteOff));
}

void test_disconnect_callback_retires_phone_work_before_main_loop_drains_event() {
    PhoneFixture f;
    f.write(muteOn);
    f.lifecycle.onDisconnect(nullptr, f.phone, 0);
    TEST_ASSERT_TRUE(f.client.proxyClientConnected_.load()); // UI state is still deferred.
    f.write(muteOn); // Callback after retirement cannot enqueue more old-phone work.
    TEST_ASSERT_EQUAL_UINT32(1, f.client.phone2v1QueueCount_);
    TEST_ASSERT_EQUAL_INT(0, f.client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL_UINT32(0, f.remote.writeValueCalls());
}

void test_inflight_old_phone_write_cannot_join_reconnected_phone_session() {
    PhoneFixture f;
    NimBLECharacteristic newInput(V1_COMMAND_WRITE_UUID);
    f.input.setGetValueEntryHook([&]() {
        f.reconnect();
        newInput.setValue(muteOff, sizeof(muteOff));
        f.writer.onWrite(&newInput, f.phone);
    });
    f.write(muteOn); // Captures old generation, resumes enqueue after new write.
    f.input.setGetValueEntryHook({});
    f.client.drainProxyCallbackEvents();
    TEST_ASSERT_EQUAL_UINT32(1, f.client.phone2v1QueueCount_);
    mockMillis += 100;
    TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL_UINT32(1, f.remote.writeValueCalls());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(muteOff, f.remote.lastWriteValue().data(), sizeof(muteOff));
}

void test_delayed_old_disconnect_event_cannot_clear_new_phone() {
    PhoneFixture f;
    f.lifecycle.onDisconnect(nullptr, f.phone, 0);
    V1BLEClient::ProxyCallbackEvent oldDisconnect{};
    TEST_ASSERT_TRUE(f.client.popProxyCallbackEvent(oldDisconnect));
    f.lifecycle.onConnect(nullptr, f.phone);
    f.client.drainProxyCallbackEvents();
    const uint32_t generation = f.client.proxyPhoneSessionGeneration_.load();
    f.client.handleProxyCallbackEvent(oldDisconnect);
    TEST_ASSERT_TRUE(f.client.proxyClientConnected_.load());
    TEST_ASSERT_EQUAL_UINT32(generation, f.client.proxyPhoneSessionGeneration_.load());
    f.write(muteOff);
    mockMillis += 100;
    TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(muteOff, f.remote.lastWriteValue().data(), sizeof(muteOff));
}

void test_other_peer_callback_cannot_retire_or_submit_new_phone_work() {
    PhoneFixture f;
    NimBLEConnInfo oldPhone = f.phone;
    f.lifecycle.onDisconnect(nullptr, oldPhone, 0);
    f.phone.setConnHandle(2);
    f.lifecycle.onConnect(nullptr, f.phone);
    f.client.drainProxyCallbackEvents();
    f.lifecycle.onDisconnect(nullptr, oldPhone, 0);
    f.input.setValue(muteOn, sizeof(muteOn));
    f.writer.onWrite(&f.input, oldPhone);
    f.client.drainProxyCallbackEvents();
    TEST_ASSERT_TRUE(f.client.proxyClientConnected_.load());
    TEST_ASSERT_EQUAL_UINT32(0, f.client.phone2v1QueueCount_);
    f.write(muteOff);
    mockMillis += 100;
    TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(muteOff, f.remote.lastWriteValue().data(), sizeof(muteOff));
}

void test_indexed_long_owner_and_pending_tail_retire_with_phone_only_disconnect() {
    PhoneFixture f;
    NimBLECharacteristic input(V1_COMMAND_WRITE_LONG_UUID);
    NimBLERemoteCharacteristic remote(V1_COMMAND_WRITE_LONG_UUID);
    f.client.pCommandCharLong_ = &remote;
    const uint8_t first[] = {0x12, 0xAA, 0xDA, 0xE6, 0x15, 0x06, 0xC0};
    const uint8_t last[] = {0x22, 0x5E, 0x56, 0x5D, 0xF2, 0x48, 0xAB};
    const uint8_t next[] = {0x11, 0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB};
    input.setValue(first, sizeof(first)); f.writer.onWrite(&input, f.phone);
    input.setValue(last, sizeof(last)); f.writer.onWrite(&input, f.phone);
    TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
    f.client.onV1DisplayFlowControl(true);
    TEST_ASSERT_EQUAL_INT(0, f.client.processPhoneCommandQueue());
    f.reconnect();
    input.setValue(last, sizeof(last)); f.writer.onWrite(&input, f.phone);
    f.client.drainProxyCallbackEvents();
    f.releaseHold();
    TEST_ASSERT_EQUAL_INT(0, f.client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL_UINT32(1, remote.writeValueCalls());
    input.setValue(next, sizeof(next)); f.writer.onWrite(&input, f.phone);
    TEST_ASSERT_EQUAL_INT(0, f.client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL_UINT32(2, remote.writeValueCalls());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(next, remote.lastWriteValue().data(), sizeof(next));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_retired_queued_or_pending_phone_mute_cannot_override_new_local_unmute);
    RUN_TEST(test_same_phone_deferral_preserves_mute_and_rejects_local_unmute);
    RUN_TEST(test_quick_phone_reconnect_keeps_new_work_and_discards_retired_pending_and_queue);
    RUN_TEST(test_disconnect_callback_retires_phone_work_before_main_loop_drains_event);
    RUN_TEST(test_inflight_old_phone_write_cannot_join_reconnected_phone_session);
    RUN_TEST(test_delayed_old_disconnect_event_cannot_clear_new_phone);
    RUN_TEST(test_other_peer_callback_cannot_retire_or_submit_new_phone_work);
    RUN_TEST(test_indexed_long_owner_and_pending_tail_retire_with_phone_only_disconnect);
    return UNITY_END();
}
