#include <unity.h>

#include "../mocks/Arduino.h"
#ifndef V1_LINKED_TEST_OBD_BLE_CLIENT
#define V1_LINKED_TEST_OBD_BLE_CLIENT
#define V1_INLINE_TEST_OBD_BLE_CLIENT
#endif
#include "../../src/modules/obd/obd_ble_client.h"
#include "../../src/modules/obd/obd_runtime_module.h"
#include "../../src/modules/obd/obd_transport_control_dispatch.h"
#ifdef V1_INLINE_TEST_OBD_BLE_CLIENT
#include "../../src/modules/obd/obd_ble_client.cpp"
#endif

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

extern "C" int ble_hs_pvcy_set_resolve_enabled(int) {
    return 0;
}

void ObdRuntimeModule::onDeviceFound(const char*, const char*, int, uint8_t) {}
void ObdRuntimeModule::onBleDisconnect(int) {}

namespace {

struct Fixture {
    ObdBleClient client;
    NimBLERemoteCharacteristic tx{"FFF1"};
    NimBLERemoteCharacteristic rx{"FFF2"};
    NimBLERemoteService service;
    NimBLEClient* nimble = nullptr;

    bool begin() {
        nimble = NimBLEDevice::createClient();
        nimble->reset();
        service.addCharacteristic(&tx);
        service.addCharacteristic(&rx);
        nimble->setService(&service);
        client.init(nullptr);
        if (!client.connect("A4:C1:38:00:11:22", 0, 5000, false)) {
            return false;
        }
        return client.discoverServices();
    }
};

struct ControlRequest {
    uint32_t requestId = 0;
    uint32_t nowMs = 0;
    uint32_t dispatchEpoch = 0;
    bool deleteBond = false;
    char address[18] = {};
    uint8_t addrType = 0;
};

void completeControl(ObdTransportControlDispatch<ControlRequest>& dispatch, QueueHandle_t controlQueue,
                     QueueHandle_t requestQueue, Fixture& fixture, uint32_t& acknowledgements) {
    auto acknowledge = [&](const ControlRequest&, bool, bool, bool, bool) { acknowledgements++; };
    fixture.nimble->emitDisconnect(534);
    TEST_ASSERT_TRUE(dispatch.service(controlQueue, requestQueue, fixture.client, acknowledge));
    TEST_ASSERT_TRUE(dispatch.service(controlQueue, requestQueue, fixture.client, acknowledge));
}

} // namespace

void setUp() {
    mock_reset_nimble_state();
}
void tearDown() {}

void test_disconnect_during_write_defers_handle_cleanup_until_operation_returns() {
    Fixture fixture;
    TEST_ASSERT_TRUE(fixture.begin());
    TEST_ASSERT_TRUE(fixture.client.hasCharacteristicHandlesForTest());

    bool hookRan = false;
    bool handlesOwnedInHook = false;
    fixture.rx.setWriteEntryHook([&]() {
        hookRan = true;
        fixture.nimble->emitDisconnect(534);
        handlesOwnedInHook = fixture.client.hasCharacteristicHandlesForTest();
    });

    TEST_ASSERT_FALSE(fixture.client.writeCommand("010D\r", false));
    TEST_ASSERT_TRUE(hookRan);
    TEST_ASSERT_TRUE(handlesOwnedInHook);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.rx.writeValueCalls());
    TEST_ASSERT_TRUE(fixture.client.hasCharacteristicHandlesForTest());
    TEST_ASSERT_EQUAL(534, fixture.client.getLastBleError());

    fixture.client.serviceDeferredLinkState();
    TEST_ASSERT_FALSE(fixture.client.hasCharacteristicHandlesForTest());
    TEST_ASSERT_FALSE(fixture.client.writeCommand("010D\r", false));
    TEST_ASSERT_EQUAL_UINT32(1, fixture.rx.writeValueCalls());
}

void test_disconnect_during_subscribe_defers_handle_cleanup_until_operation_returns() {
    Fixture fixture;
    TEST_ASSERT_TRUE(fixture.begin());
    TEST_ASSERT_TRUE(fixture.client.hasCharacteristicHandlesForTest());

    bool hookRan = false;
    bool handlesOwnedInHook = false;
    fixture.tx.setSubscribeEntryHook([&]() {
        hookRan = true;
        fixture.nimble->emitDisconnect(534);
        handlesOwnedInHook = fixture.client.hasCharacteristicHandlesForTest();
    });

    TEST_ASSERT_FALSE(fixture.client.subscribeNotify(nullptr));
    TEST_ASSERT_TRUE(hookRan);
    TEST_ASSERT_TRUE(handlesOwnedInHook);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.tx.subscribeCalls());
    TEST_ASSERT_TRUE(fixture.client.hasCharacteristicHandlesForTest());
    TEST_ASSERT_EQUAL(534, fixture.client.getLastBleError());

    fixture.client.serviceDeferredLinkState();
    TEST_ASSERT_FALSE(fixture.client.hasCharacteristicHandlesForTest());
    TEST_ASSERT_FALSE(fixture.client.subscribeNotify(nullptr));
    TEST_ASSERT_EQUAL_UINT32(1, fixture.tx.subscribeCalls());
}

void test_control_disconnect_waits_for_claimed_real_write() {
    Fixture fixture;
    TEST_ASSERT_TRUE(fixture.begin());

    QueueHandle_t controlQueue = xQueueCreate(1, sizeof(ControlRequest));
    QueueHandle_t requestQueue = xQueueCreate(1, sizeof(ControlRequest));
    ObdTransportRequestEpoch epoch;
    ObdTransportControlDispatch<ControlRequest> dispatch;
    ControlRequest control{};
    control.requestId = 31;

    bool writeReturned = false;
    bool disconnectObservedAfterWrite = false;
    fixture.nimble->setDisconnectEntryHook([&]() { disconnectObservedAfterWrite = writeReturned; });
    fixture.rx.setWriteEntryHook([&]() {
        epoch.cancelQueuedWork();
        TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(controlQueue, &control, 0));
        TEST_ASSERT_TRUE(fixture.client.hasCharacteristicHandlesForTest());
        TEST_ASSERT_EQUAL_UINT32(0, fixture.nimble->disconnectCalls());
    });

    const uint32_t requestEpoch = epoch.snapshot();
    TEST_ASSERT_TRUE(epoch.tryClaim(requestEpoch));
    TEST_ASSERT_TRUE(fixture.client.writeCommand("010D\r", false));
    writeReturned = true;
    epoch.releaseClaim();

    uint32_t acknowledgements = 0;
    auto acknowledge = [&](const ControlRequest&, bool, bool, bool, bool) { acknowledgements++; };
    TEST_ASSERT_TRUE(dispatch.service(controlQueue, requestQueue, fixture.client, acknowledge));
    TEST_ASSERT_TRUE(disconnectObservedAfterWrite);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.rx.writeValueCalls());
    TEST_ASSERT_EQUAL_UINT32(0, acknowledgements);

    completeControl(dispatch, controlQueue, requestQueue, fixture, acknowledgements);
    TEST_ASSERT_EQUAL_UINT32(1, acknowledgements);
    TEST_ASSERT_FALSE(fixture.client.hasCharacteristicHandlesForTest());

    vQueueDelete(controlQueue);
    vQueueDelete(requestQueue);
}

void test_control_disconnect_invalidates_real_write_before_claim() {
    Fixture fixture;
    TEST_ASSERT_TRUE(fixture.begin());

    QueueHandle_t controlQueue = xQueueCreate(1, sizeof(ControlRequest));
    QueueHandle_t requestQueue = xQueueCreate(1, sizeof(ControlRequest));
    ObdTransportRequestEpoch epoch;
    ObdTransportControlDispatch<ControlRequest> dispatch;
    ControlRequest control{};

    const uint32_t staleEpoch = epoch.snapshot();
    epoch.cancelQueuedWork();
    TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(controlQueue, &control, 0));
    TEST_ASSERT_FALSE(epoch.tryClaim(staleEpoch));
    TEST_ASSERT_EQUAL_UINT32(0, fixture.rx.writeValueCalls());

    uint32_t acknowledgements = 0;
    auto acknowledge = [&](const ControlRequest&, bool, bool, bool, bool) { acknowledgements++; };
    TEST_ASSERT_TRUE(dispatch.service(controlQueue, requestQueue, fixture.client, acknowledge));
    TEST_ASSERT_EQUAL_UINT32(0, fixture.rx.writeValueCalls());
    TEST_ASSERT_EQUAL_UINT32(1, fixture.nimble->disconnectCalls());

    completeControl(dispatch, controlQueue, requestQueue, fixture, acknowledgements);
    TEST_ASSERT_EQUAL_UINT32(1, acknowledgements);

    vQueueDelete(controlQueue);
    vQueueDelete(requestQueue);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_disconnect_during_write_defers_handle_cleanup_until_operation_returns);
    RUN_TEST(test_disconnect_during_subscribe_defers_handle_cleanup_until_operation_returns);
    RUN_TEST(test_control_disconnect_waits_for_claimed_real_write);
    RUN_TEST(test_control_disconnect_invalidates_real_write_before_claim);
    return UNITY_END();
}
