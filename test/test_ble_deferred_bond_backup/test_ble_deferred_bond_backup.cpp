#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/FS.h"
#include "../mocks/NimBLEDevice.h"
#include "../mocks/storage_manager.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#define private public
#include "../../src/ble_client.h"
#undef private

portMUX_TYPE pendingAddrMux = portMUX_INITIALIZER_UNLOCKED;
V1BLEClient* instancePtr = nullptr;

namespace {

fs::FS g_sdFs(std::filesystem::temp_directory_path() / "v1simple_ble_deferred_bond_backup");
int g_tryBackupCalls = 0;
int g_tryBackupResult = 1;
int g_requestAlertCalls = 0;
int g_requestVersionCalls = 0;
bool g_requestAlertResult = true;
bool g_requestVersionResult = true;
int g_stableCallbackCalls = 0;
uint32_t g_lastAlertFollowupUs = 0;
uint32_t g_lastVersionFollowupUs = 0;
uint32_t g_lastStableCallbackUs = 0;

struct CountingSerial {
    uint32_t printlnCount = 0;

    void reset() { printlnCount = 0; }
    void println(const char* = "") { printlnCount++; }
};

CountingSerial countedSerial;

void stableConnectCallback() {
    ++g_stableCallbackCalls;
    mockMicros += 31;
}

}  // namespace

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

bool V1BLEClient::requestAlertData() {
    ++g_requestAlertCalls;
    mockMicros += 17;
    return g_requestAlertResult;
}
bool V1BLEClient::requestVersion() {
    ++g_requestVersionCalls;
    mockMicros += 19;
    return g_requestVersionResult;
}
bool V1BLEClient::requestAllVolume() {
    // Test stub for requestAllVolume(): real impl sends 0x3C; here we
    // just record a tick so the followup chain can exercise both calls.
    mockMicros += 11;
    return true;
}
bool V1BLEClient::isConnected() { return connected_.load(std::memory_order_relaxed); }
int V1BLEClient::processPhoneCommandQueue() { return 0; }
void V1BLEClient::setBLEState(BLEState newState, const char*) { bleState_ = newState; }
void V1BLEClient::processConnectingWait() {}
void V1BLEClient::processDiscovering() {}
void V1BLEClient::processSubscribing() {}
void V1BLEClient::processSubscribeYield() {}
void V1BLEClient::releaseProxyQueues() {}
void V1BLEClient::startProxyAdvertising(uint8_t, bool) {}

void perfRecordBleFollowupRequestAlertUs(uint32_t us) { g_lastAlertFollowupUs = us; }
void perfRecordBleFollowupRequestVersionUs(uint32_t us) { g_lastVersionFollowupUs = us; }
void perfRecordBleConnectStableCallbackUs(uint32_t us) { g_lastStableCallbackUs = us; }

int V1BLEClient::tryBackupBondsToSD() {
    g_tryBackupCalls++;
    StorageManager::SDTryLock lock(storageManager.getSDMutex(), false);
    if (!lock) {
        return -1;
    }
    return g_tryBackupResult;
}

#define Serial countedSerial
#include "../../src/ble_connected_followup.cpp"
#undef Serial

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
    g_tryBackupCalls = 0;
    g_tryBackupResult = 1;
    g_requestAlertCalls = 0;
    g_requestVersionCalls = 0;
    g_requestAlertResult = true;
    g_requestVersionResult = true;
    g_stableCallbackCalls = 0;
    g_lastAlertFollowupUs = 0;
    g_lastVersionFollowupUs = 0;
    g_lastStableCallbackUs = 0;
    countedSerial.reset();
    mock_reset_nimble_state();
    StorageManager::resetMockSdLockState();
    storageManager.reset();
    storageManager.setFilesystem(&g_sdFs, true);
}

void tearDown() {}

void test_followup_backup_step_marks_pending_without_inline_write() {
    V1BLEClient client;
    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::BACKUP_BONDS;
    client.lastBondBackupCount_ = 1;
    g_mock_nimble_state.bondCount = 2;

    client.processConnectedFollowup();

    TEST_ASSERT_EQUAL_INT(static_cast<int>(V1BLEClient::ConnectedFollowupStep::NONE),
                          static_cast<int>(client.connectedFollowupStep_));
    TEST_ASSERT_TRUE(client.pendingBondBackup_);
    TEST_ASSERT_EQUAL_UINT8(2, client.pendingBondBackupCount_);
    TEST_ASSERT_EQUAL_UINT32(0, client.pendingBondBackupRetryAtMs_);
    TEST_ASSERT_EQUAL(0, g_tryBackupCalls);
    TEST_ASSERT_EQUAL_UINT32(0, StorageManager::mockSdLockState.tryAcquireCalls);
    TEST_ASSERT_EQUAL_UINT32(0, StorageManager::mockSdLockState.blockingAcquireCalls);
}

void test_followup_backup_step_noops_when_bond_count_is_already_backed_up() {
    V1BLEClient client;
    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::BACKUP_BONDS;
    client.lastBondBackupCount_ = 3;
    g_mock_nimble_state.bondCount = 3;

    client.processConnectedFollowup();

    TEST_ASSERT_FALSE(client.pendingBondBackup_);
    TEST_ASSERT_EQUAL_UINT8(0xFF, client.pendingBondBackupCount_);
    TEST_ASSERT_EQUAL(0, g_tryBackupCalls);
}

void test_service_deferred_bond_backup_retries_after_trylock_busy() {
    V1BLEClient client;
    client.pendingBondBackup_ = true;
    client.pendingBondBackupCount_ = 4;
    client.lastBondBackupCount_ = 3;
    StorageManager::mockSdLockState.failNextTryLockCount = 1;

    client.serviceDeferredBondBackup(1000);

    TEST_ASSERT_TRUE(client.pendingBondBackup_);
    TEST_ASSERT_EQUAL_UINT8(3, client.lastBondBackupCount_);
    TEST_ASSERT_EQUAL_UINT32(2000, client.pendingBondBackupRetryAtMs_);
    TEST_ASSERT_EQUAL(1, g_tryBackupCalls);
    TEST_ASSERT_EQUAL_UINT32(1, StorageManager::mockSdLockState.tryAcquireCalls);

    client.serviceDeferredBondBackup(1500);
    TEST_ASSERT_EQUAL(1, g_tryBackupCalls);
    TEST_ASSERT_TRUE(client.pendingBondBackup_);
}

void test_service_deferred_bond_backup_success_clears_pending_and_updates_count() {
    V1BLEClient client;
    client.pendingBondBackup_ = true;
    client.pendingBondBackupCount_ = 5;
    client.lastBondBackupCount_ = 2;
    g_tryBackupResult = 2;

    client.serviceDeferredBondBackup(5000);

    TEST_ASSERT_FALSE(client.pendingBondBackup_);
    TEST_ASSERT_EQUAL_UINT8(5, client.lastBondBackupCount_);
    TEST_ASSERT_EQUAL_UINT32(0, client.pendingBondBackupRetryAtMs_);
    TEST_ASSERT_EQUAL(1, g_tryBackupCalls);
    TEST_ASSERT_EQUAL_UINT32(1, StorageManager::mockSdLockState.tryAcquireCalls);
    TEST_ASSERT_EQUAL_UINT32(0, StorageManager::mockSdLockState.blockingAcquireCalls);
}

void test_alert_request_stays_on_critical_path_before_settle_gate() {
    V1BLEClient client;
    client.connected_.store(true, std::memory_order_relaxed);
    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::REQUEST_ALERT_DATA;

    client.processConnectedFollowup();

    TEST_ASSERT_EQUAL(1, g_requestAlertCalls);
    TEST_ASSERT_TRUE(g_lastAlertFollowupUs > 0);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(V1BLEClient::ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE),
        static_cast<int>(client.connectedFollowupStep_));
}

void test_followup_alert_failure_log_is_rate_limited() {
    V1BLEClient client;
    client.connected_.store(true, std::memory_order_relaxed);
    g_requestAlertResult = false;

    mockMillis = 1000;
    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::REQUEST_ALERT_DATA;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_UINT32(1, countedSerial.printlnCount);

    mockMillis = 2000;
    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::REQUEST_ALERT_DATA;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_UINT32(1, countedSerial.printlnCount);

    mockMillis = 11000;
    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::REQUEST_ALERT_DATA;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_UINT32(2, countedSerial.printlnCount);
}

void test_stable_callback_waits_for_settle_gate_and_proxy_start_follows_callback() {
    V1BLEClient client;
    client.connected_.store(true, std::memory_order_relaxed);
    client.proxyEnabled_ = true;
    client.proxyServerInitialized_ = true;
    client.connectStableCallback_ = stableConnectCallback;
    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE;
    client.connectCompletedAtMs_.store(1000, std::memory_order_relaxed);
    client.firstRxAfterConnectMs_.store(1100, std::memory_order_relaxed);
    client.lastBleProcessDurationUs_.store(20000, std::memory_order_relaxed);
    client.lastDisplayPipelineDurationUs_.store(40000, std::memory_order_relaxed);

    mockMillis = 1200;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(V1BLEClient::ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE),
        static_cast<int>(client.connectedFollowupStep_));
    TEST_ASSERT_EQUAL(0, g_stableCallbackCalls);
    TEST_ASSERT_EQUAL_UINT32(0, client.proxyAdvertisingStartMs_);

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(V1BLEClient::ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE),
        static_cast<int>(client.connectedFollowupStep_));

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(V1BLEClient::ConnectedFollowupStep::REQUEST_VERSION),
        static_cast<int>(client.connectedFollowupStep_));
    TEST_ASSERT_EQUAL(0, g_requestVersionCalls);
    TEST_ASSERT_EQUAL(0, g_stableCallbackCalls);
    TEST_ASSERT_EQUAL_UINT32(0, client.proxyAdvertisingStartMs_);

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(1, g_requestVersionCalls);
    TEST_ASSERT_TRUE(g_lastVersionFollowupUs > 0);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(V1BLEClient::ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK),
        static_cast<int>(client.connectedFollowupStep_));

    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL(1, g_stableCallbackCalls);
    TEST_ASSERT_TRUE(g_lastStableCallbackUs > 0);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(V1BLEClient::ConnectedFollowupStep::BACKUP_BONDS),
        static_cast<int>(client.connectedFollowupStep_));
    TEST_ASSERT_EQUAL_UINT32(0, client.proxyAdvertisingStartMs_);

    mockMillis = 1300;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(V1BLEClient::ConnectedFollowupStep::NONE),
        static_cast<int>(client.connectedFollowupStep_));
    TEST_ASSERT_EQUAL_UINT32(0, client.proxyAdvertisingStartMs_);
}

void test_settle_gate_times_out_without_first_rx() {
    V1BLEClient client;
    client.connected_.store(true, std::memory_order_relaxed);
    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE;
    client.connectCompletedAtMs_.store(1000, std::memory_order_relaxed);
    client.firstRxAfterConnectMs_.store(0, std::memory_order_relaxed);
    client.lastBleProcessDurationUs_.store(60000, std::memory_order_relaxed);
    client.lastDisplayPipelineDurationUs_.store(70000, std::memory_order_relaxed);

    mockMillis = 3499;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(V1BLEClient::ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE),
        static_cast<int>(client.connectedFollowupStep_));

    mockMillis = 3500;
    client.processConnectedFollowup();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(V1BLEClient::ConnectedFollowupStep::REQUEST_VERSION),
        static_cast<int>(client.connectedFollowupStep_));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_followup_backup_step_marks_pending_without_inline_write);
    RUN_TEST(test_followup_backup_step_noops_when_bond_count_is_already_backed_up);
    RUN_TEST(test_service_deferred_bond_backup_retries_after_trylock_busy);
    RUN_TEST(test_service_deferred_bond_backup_success_clears_pending_and_updates_count);
    RUN_TEST(test_alert_request_stays_on_critical_path_before_settle_gate);
    RUN_TEST(test_followup_alert_failure_log_is_rate_limited);
    RUN_TEST(test_stable_callback_waits_for_settle_gate_and_proxy_start_follows_callback);
    RUN_TEST(test_settle_gate_times_out_without_first_rx);
    return UNITY_END();
}
