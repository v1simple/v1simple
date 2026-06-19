#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#define private public
#include "../../src/ble_client.h"
#undef private

#include "../../src/perf_metrics.h"

portMUX_TYPE pendingAddrMux = portMUX_INITIALIZER_UNLOCKED;
V1BLEClient* instancePtr = nullptr;
PerfCounters perfCounters;

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
    , connectStableCallback_(nullptr)
    , hasTargetDevice_(false)
    , targetAddress_()
    , lastScanStart_(0)
    , freshFlashBoot_(false)
    , pScanCallbacks_(nullptr)
    , pClientCallbacks_(nullptr)
    , pProxyServerCallbacks_(nullptr)
    , pProxyWriteCallbacks_(nullptr) {
    instancePtr = this;
}

V1BLEClient::~V1BLEClient() {}

void V1BLEClient::ClientCallbacks::onConnect(NimBLEClient*) {}
void V1BLEClient::ClientCallbacks::onDisconnect(NimBLEClient*, int) {}
void V1BLEClient::ClientCallbacks::onPhyUpdate(NimBLEClient*, uint8_t, uint8_t) {}
void V1BLEClient::ScanCallbacks::onResult(const NimBLEAdvertisedDevice*) {}
void V1BLEClient::ScanCallbacks::onScanEnd(const NimBLEScanResults&, int) {}
void V1BLEClient::ProxyServerCallbacks::onConnect(NimBLEServer*, NimBLEConnInfo&) {}
void V1BLEClient::ProxyServerCallbacks::onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {}
void V1BLEClient::ProxyWriteCallbacks::onWrite(NimBLECharacteristic*, NimBLEConnInfo&) {}

bool V1BLEClient::isConnected() {
    return connected_.load(std::memory_order_relaxed);
}

#include "../../src/ble_commands.cpp"

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
}

void tearDown() {}

void test_verify_push_match_edge_is_consumed_once() {
    V1BLEClient client;
    const uint8_t expected[6] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15};

    client.startUserBytesVerification(expected);
    client.onUserBytesReceived(expected);

    TEST_ASSERT_TRUE(client.verifyComplete_);
    TEST_ASSERT_TRUE(client.verifyMatch_);
    TEST_ASSERT_TRUE(client.consumeVerifyPushMatchEdge());
    TEST_ASSERT_FALSE(client.consumeVerifyPushMatchEdge());
}

void test_verify_push_mismatch_does_not_raise_edge() {
    V1BLEClient client;
    const uint8_t expected[6] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25};
    const uint8_t actual[6] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x26};

    client.startUserBytesVerification(expected);
    client.onUserBytesReceived(actual);

    TEST_ASSERT_TRUE(client.verifyComplete_);
    TEST_ASSERT_FALSE(client.verifyMatch_);
    TEST_ASSERT_FALSE(client.consumeVerifyPushMatchEdge());
}

void test_start_verification_clears_stale_match_edge() {
    V1BLEClient client;
    const uint8_t expected[6] = {0x30, 0x31, 0x32, 0x33, 0x34, 0x35};

    client.startUserBytesVerification(expected);
    client.onUserBytesReceived(expected);
    TEST_ASSERT_TRUE(client.consumeVerifyPushMatchEdge());

    client.startUserBytesVerification(expected);

    TEST_ASSERT_FALSE(client.consumeVerifyPushMatchEdge());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_verify_push_match_edge_is_consumed_once);
    RUN_TEST(test_verify_push_mismatch_does_not_raise_edge);
    RUN_TEST(test_start_verification_clears_stale_match_edge);
    return UNITY_END();
}
