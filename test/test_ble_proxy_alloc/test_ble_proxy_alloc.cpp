#include <unity.h>
#include <atomic>
#include <thread>
#include <type_traits>
#include <vector>

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
#define configASSERT(expr) do { if (!(expr)) { TEST_FAIL_MESSAGE("configASSERT failed"); } } while (0)
#endif

#define private public
#include "../../src/ble_client.h"
#undef private

#include "../../src/perf_metrics.h"

PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;

void perfRecordBleProxyStartUs(uint32_t) {}
void perfRecordBleConnectUs(uint32_t) {}
void perfRecordBleDiscoveryUs(uint32_t) {}
void perfRecordBleSubscribeUs(uint32_t) {}
void perfRecordProxyAdvertisingTransition(bool, uint8_t, uint32_t) {}
const char* perfConnectionCycleStateName(uint8_t) { return "UNKNOWN"; }

portMUX_TYPE pendingAddrMux = portMUX_INITIALIZER_UNLOCKED;
V1BLEClient* instancePtr = nullptr;

namespace {

SendResult g_sendCommandResult = SendResult::SENT;
std::vector<uint8_t> g_lastSentCommand;
std::vector<uint8_t> g_sentCommandHistory;

void resetPhoneCommandSendState() {
    g_sendCommandResult = SendResult::SENT;
    g_lastSentCommand.clear();
    g_sentCommandHistory.clear();
}

void assertPhoneCmdDropMetrics(const V1BLEClient& client,
                               uint32_t overflow,
                               uint32_t invalid,
                               uint32_t bleFail,
                               uint32_t lockBusy) {
    const PhoneCmdDropMetricsSnapshot snapshot = perfPhoneCmdDropMetricsSnapshot();
    JsonDocument doc;
    perfAppendPhoneCmdDropMetrics(doc, snapshot);

    TEST_ASSERT_EQUAL_UINT32(overflow, client.getPhoneCmdDropsOverflow());
    TEST_ASSERT_EQUAL_UINT32(invalid, client.getPhoneCmdDropsInvalid());
    TEST_ASSERT_EQUAL_UINT32(bleFail, client.getPhoneCmdDropsBleFail());
    TEST_ASSERT_EQUAL_UINT32(lockBusy, client.getPhoneCmdDropsLockBusy());
    TEST_ASSERT_EQUAL_UINT32(overflow, doc["phoneCmdDropsOverflow"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(invalid, doc["phoneCmdDropsInvalid"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(bleFail, doc["phoneCmdDropsBleFail"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(lockBusy, doc["phoneCmdDropsLockBusy"].as<uint32_t>());
}

SemaphoreHandle_t g_releaseNotifyMutex = nullptr;
SemaphoreHandle_t g_releasePhoneMutex = nullptr;
bool g_observeProxyQueueFree = false;
bool g_releaseLocksHeldDuringEveryFree = true;
uint32_t g_observedProxyQueueFreeCalls = 0;

void observeHeapCapsFree(void* ptr) {
    (void)ptr;
    if (g_observeProxyQueueFree) {
        ++g_observedProxyQueueFreeCalls;
        g_releaseLocksHeldDuringEveryFree =
            g_releaseLocksHeldDuringEveryFree &&
            mock_semaphore_is_held(g_releaseNotifyMutex) &&
            mock_semaphore_is_held(g_releasePhoneMutex);
    }
}

void observeProxyQueueRelease(V1BLEClient& client) {
    g_releaseNotifyMutex = reinterpret_cast<SemaphoreHandle_t>(0xB1E0u);
    g_releasePhoneMutex = reinterpret_cast<SemaphoreHandle_t>(0xB1E1u);
    client.bleNotifyMutex_ = g_releaseNotifyMutex;
    client.phoneCmdMutex_ = g_releasePhoneMutex;
    g_observeProxyQueueFree = true;
    g_mock_heap_caps_free_observer = observeHeapCapsFree;
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

V1BLEClient::~V1BLEClient() {
    releaseProxyQueues();
}

void V1BLEClient::setBLEState(BLEState newState, const char*) {
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
    setBLEState(BLEState::DISCONNECTED, "hard reset test stub");
}

void V1BLEClient::cleanupConnection() {}
void V1BLEClient::completeHardResetBLEClient() {}

void V1BLEClient::setProxyClientConnected(bool connectedState) {
    proxyClientConnected_ = connectedState;
    if (connectedState) {
        proxyClientConnectedOnceThisBoot_ = true;
        proxyFastAdvertisingUntilMs_ = 0;
    }
}

SendResult V1BLEClient::sendCommandWithResult(const uint8_t* data, size_t length) {
    if (data && length > 0) {
        g_lastSentCommand.assign(data, data + length);
        g_sentCommandHistory.push_back(data[0]);
    } else {
        g_lastSentCommand.clear();
    }
    return g_sendCommandResult;
}

#if !defined(V1_LINKED_TEST_BLE_PROXY_ALLOC)
#include "../../src/ble_proxy.cpp"
#endif
#include "../../src/ble_connection.cpp"

void setUp() {
    mock_reset_heap_caps();
    mock_reset_semaphore_state();
    mock_reset_nimble_state();
    mockMillis = 0;
    mockMicros = 0;
    perfCounters.reset();
    perfExtended.reset();
    resetPhoneCommandSendState();
    g_releaseNotifyMutex = nullptr;
    g_releasePhoneMutex = nullptr;
    g_mock_heap_caps_free_observer = nullptr;
    g_observeProxyQueueFree = false;
    g_releaseLocksHeldDuringEveryFree = true;
    g_observedProxyQueueFreeCalls = 0;
}

void tearDown() {}

void test_ble_timing_members_and_constants_use_uint32() {
    V1BLEClient client;

    TEST_ASSERT_TRUE((std::is_same_v<decltype(client.lastScanStart_), uint32_t>));
    TEST_ASSERT_TRUE((std::is_same_v<decltype(client.stateEnteredMs_), uint32_t>));
    TEST_ASSERT_TRUE((std::is_same_v<decltype(client.scanStopRequestedMs_), uint32_t>));
    TEST_ASSERT_TRUE((std::is_same_v<decltype(client.connectStartMs_), uint32_t>));
    TEST_ASSERT_TRUE((std::is_same_v<decltype(client.nextConnectAllowedMs_), uint32_t>));
    TEST_ASSERT_TRUE((std::is_same_v<decltype(client.proxyAdvertisingStartMs_), uint32_t>));
    TEST_ASSERT_TRUE((std::is_same_v<decltype(client.proxyFastAdvertisingUntilMs_), uint32_t>));

    TEST_ASSERT_TRUE((std::is_same_v<std::remove_cv_t<decltype(V1BLEClient::SCAN_STOP_SETTLE_MS)>, uint32_t>));
    TEST_ASSERT_TRUE((std::is_same_v<std::remove_cv_t<decltype(V1BLEClient::SCAN_STOP_SETTLE_FRESH_MS)>, uint32_t>));
    TEST_ASSERT_TRUE((std::is_same_v<std::remove_cv_t<decltype(V1BLEClient::CONNECT_TIMEOUT_MS)>, uint32_t>));
    TEST_ASSERT_TRUE((std::is_same_v<std::remove_cv_t<decltype(V1BLEClient::DISCOVERY_TIMEOUT_MS)>, uint32_t>));
    TEST_ASSERT_TRUE((std::is_same_v<std::remove_cv_t<decltype(V1BLEClient::SUBSCRIBE_TIMEOUT_MS)>, uint32_t>));
    TEST_ASSERT_TRUE((std::is_same_v<std::remove_cv_t<decltype(V1BLEClient::PROXY_STABILIZE_MS)>, uint32_t>));
    TEST_ASSERT_TRUE((std::is_same_v<std::remove_cv_t<decltype(V1BLEClient::PROXY_FAST_START_WINDOW_MS)>, uint32_t>));
    TEST_ASSERT_TRUE((std::is_same_v<std::remove_cv_t<decltype(V1BLEClient::PROXY_FAST_RECONNECT_WINDOW_MS)>, uint32_t>));
    TEST_ASSERT_TRUE((std::is_same_v<std::remove_cv_t<decltype(V1BLEClient::PROXY_ADV_FAST_MIN_INTERVAL)>, uint16_t>));
    TEST_ASSERT_TRUE((std::is_same_v<std::remove_cv_t<decltype(V1BLEClient::PROXY_ADV_SLOW_MIN_INTERVAL)>, uint16_t>));
}

void test_allocateProxyQueues_prefers_psram_for_both_buffers() {
    V1BLEClient client;

    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    TEST_ASSERT_NOT_NULL(client.proxyQueue_);
    TEST_ASSERT_NOT_NULL(client.phone2v1Queue_);
    TEST_ASSERT_TRUE(client.proxyQueuesInPsram_);
    TEST_ASSERT_EQUAL_UINT32(2, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_EQUAL_UINT32(0, client.proxyQueueCount_);
    TEST_ASSERT_EQUAL_UINT32(0, client.phone2v1QueueCount_);
}

void test_allocateProxyQueues_falls_back_to_internal_when_psram_misses() {
    V1BLEClient client;
    g_mock_heap_caps_fail_on_call = 2u;

    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    TEST_ASSERT_NOT_NULL(client.proxyQueue_);
    TEST_ASSERT_NOT_NULL(client.phone2v1Queue_);
    TEST_ASSERT_FALSE(client.proxyQueuesInPsram_);
    TEST_ASSERT_EQUAL_UINT32(3, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL, g_mock_heap_caps_last_malloc_caps);
}

void test_releaseProxyQueues_defers_when_notify_mutex_is_busy() {
    V1BLEClient client;
    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    observeProxyQueueRelease(client);
    auto* const proxyQueue = client.proxyQueue_;
    auto* const phoneQueue = client.phone2v1Queue_;
    mock_queue_semaphore_take_result(pdFALSE);

    client.releaseProxyQueues();

    TEST_ASSERT_EQUAL_PTR(proxyQueue, client.proxyQueue_);
    TEST_ASSERT_EQUAL_PTR(phoneQueue, client.phone2v1Queue_);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_heap_caps_free_calls);
    TEST_ASSERT_EQUAL_UINT32(2, g_mock_heap_caps_outstanding_allocations);
    TEST_ASSERT_TRUE(client.proxyQueueReleasePending_.load(std::memory_order_acquire));
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_semaphore_state.takeCalls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_semaphore_state.giveCalls);
    TEST_ASSERT_EQUAL_PTR(g_releaseNotifyMutex, g_mock_semaphore_state.takeHandles[0]);
}

void test_releaseProxyQueues_defers_when_phone_mutex_is_busy() {
    V1BLEClient client;
    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    observeProxyQueueRelease(client);
    auto* const proxyQueue = client.proxyQueue_;
    auto* const phoneQueue = client.phone2v1Queue_;
    mock_queue_semaphore_take_result(pdTRUE);
    mock_queue_semaphore_take_result(pdFALSE);

    client.releaseProxyQueues();

    TEST_ASSERT_EQUAL_PTR(proxyQueue, client.proxyQueue_);
    TEST_ASSERT_EQUAL_PTR(phoneQueue, client.phone2v1Queue_);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_heap_caps_free_calls);
    TEST_ASSERT_EQUAL_UINT32(2, g_mock_heap_caps_outstanding_allocations);
    TEST_ASSERT_TRUE(client.proxyQueueReleasePending_.load(std::memory_order_acquire));
    TEST_ASSERT_EQUAL_UINT32(2, g_mock_semaphore_state.takeCalls);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_semaphore_state.giveCalls);
    TEST_ASSERT_EQUAL_PTR(g_releaseNotifyMutex, g_mock_semaphore_state.takeHandles[0]);
    TEST_ASSERT_EQUAL_PTR(g_releasePhoneMutex, g_mock_semaphore_state.takeHandles[1]);
    TEST_ASSERT_EQUAL_PTR(g_releaseNotifyMutex, g_mock_semaphore_state.giveHandles[0]);
    TEST_ASSERT_FALSE(mock_semaphore_is_held(g_releaseNotifyMutex));
}

void test_releaseProxyQueues_retry_frees_after_busy_mutex_clears() {
    V1BLEClient client;
    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    observeProxyQueueRelease(client);
    mock_queue_semaphore_take_result(pdTRUE);
    mock_queue_semaphore_take_result(pdFALSE);
    client.releaseProxyQueues();
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_heap_caps_free_calls);
    TEST_ASSERT_TRUE(client.proxyQueueReleasePending_.load(std::memory_order_acquire));

    mock_queue_semaphore_take_result(pdTRUE);
    mock_queue_semaphore_take_result(pdTRUE);
    TEST_ASSERT_TRUE(client.tryFinalizeProxyQueueRelease());

    TEST_ASSERT_NULL(client.proxyQueue_);
    TEST_ASSERT_NULL(client.phone2v1Queue_);
    TEST_ASSERT_EQUAL_UINT32(2, g_mock_heap_caps_free_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_heap_caps_outstanding_allocations);
    TEST_ASSERT_FALSE(client.proxyQueueReleasePending_.load(std::memory_order_acquire));
    TEST_ASSERT_EQUAL_UINT32(2, g_observedProxyQueueFreeCalls);
    TEST_ASSERT_TRUE(g_releaseLocksHeldDuringEveryFree);
    TEST_ASSERT_FALSE(mock_semaphore_is_held(g_releaseNotifyMutex));
    TEST_ASSERT_FALSE(mock_semaphore_is_held(g_releasePhoneMutex));
}

void test_releaseProxyQueues_holds_both_mutexes_and_repeated_release_is_harmless() {
    V1BLEClient client;
    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    observeProxyQueueRelease(client);
    mock_queue_semaphore_take_result(pdTRUE);
    mock_queue_semaphore_take_result(pdTRUE);

    client.releaseProxyQueues();

    TEST_ASSERT_NULL(client.proxyQueue_);
    TEST_ASSERT_NULL(client.phone2v1Queue_);
    TEST_ASSERT_EQUAL_UINT32(2, g_observedProxyQueueFreeCalls);
    TEST_ASSERT_TRUE(g_releaseLocksHeldDuringEveryFree);
    TEST_ASSERT_EQUAL_UINT32(2, g_mock_semaphore_state.takeCalls);
    TEST_ASSERT_EQUAL_UINT32(2, g_mock_semaphore_state.giveCalls);
    TEST_ASSERT_EQUAL_PTR(g_releaseNotifyMutex, g_mock_semaphore_state.takeHandles[0]);
    TEST_ASSERT_EQUAL_PTR(g_releasePhoneMutex, g_mock_semaphore_state.takeHandles[1]);

    client.releaseProxyQueues();

    TEST_ASSERT_EQUAL_UINT32(2, g_mock_heap_caps_free_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_heap_caps_outstanding_allocations);
    TEST_ASSERT_EQUAL_UINT32(2, g_mock_semaphore_state.takeCalls);
    TEST_ASSERT_EQUAL_UINT32(2, g_mock_semaphore_state.giveCalls);
    TEST_ASSERT_EQUAL_UINT32(2, g_observedProxyQueueFreeCalls);
}

void test_pending_release_gates_queue_producers_and_consumers() {
    V1BLEClient client;
    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    observeProxyQueueRelease(client);
    client.proxyEnabled_ = true;
    client.proxyClientConnected_.store(true, std::memory_order_relaxed);
    client.connected_.store(true, std::memory_order_relaxed);
    const uint8_t notify[] = {0xAA, 0x55, 0x10};
    const uint8_t command[] = {0x11};
    TEST_ASSERT_TRUE(client.enqueuePhoneCommand(command, sizeof(command), 0xB2CE));
    client.forwardToProxy(notify, sizeof(notify), 0xB2CE);
    TEST_ASSERT_EQUAL_UINT32(1, client.phone2v1QueueCount_);
    TEST_ASSERT_EQUAL_UINT32(1, client.proxyQueueCount_);

    mock_queue_semaphore_take_result(pdFALSE);
    client.releaseProxyQueues();
    TEST_ASSERT_TRUE(client.proxyQueueReleasePending_.load(std::memory_order_acquire));

    TEST_ASSERT_FALSE(client.enqueuePhoneCommand(command, sizeof(command), 0xB2CE));
    client.forwardToProxy(notify, sizeof(notify), 0xB2CE);
    TEST_ASSERT_EQUAL_INT(0, client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL_INT(0, client.processProxyQueue());
    TEST_ASSERT_EQUAL_UINT32(1, client.phone2v1QueueCount_);
    TEST_ASSERT_EQUAL_UINT32(1, client.proxyQueueCount_);

    mock_queue_semaphore_take_result(pdTRUE);
    mock_queue_semaphore_take_result(pdTRUE);
    TEST_ASSERT_TRUE(client.tryFinalizeProxyQueueRelease());
}

void test_allocateProxyQueues_does_not_overwrite_a_pending_release() {
    V1BLEClient client;
    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    observeProxyQueueRelease(client);
    auto* const proxyQueue = client.proxyQueue_;
    auto* const phoneQueue = client.phone2v1Queue_;
    const uint32_t mallocCalls = g_mock_heap_caps_malloc_calls;

    mock_queue_semaphore_take_result(pdFALSE);
    client.releaseProxyQueues();
    mock_queue_semaphore_take_result(pdFALSE);

    TEST_ASSERT_FALSE(client.allocateProxyQueues());
    TEST_ASSERT_EQUAL_PTR(proxyQueue, client.proxyQueue_);
    TEST_ASSERT_EQUAL_PTR(phoneQueue, client.phone2v1Queue_);
    TEST_ASSERT_EQUAL_UINT32(mallocCalls, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_heap_caps_free_calls);

    mock_queue_semaphore_take_result(pdTRUE);
    mock_queue_semaphore_take_result(pdTRUE);
    TEST_ASSERT_TRUE(client.tryFinalizeProxyQueueRelease());
}

void test_reallocated_queues_reject_callbacks_from_the_previous_epoch() {
    V1BLEClient client;
    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    observeProxyQueueRelease(client);
    client.proxyEnabled_ = true;
    client.proxyClientConnected_.store(true, std::memory_order_relaxed);
    const uint32_t staleEpoch = client.proxyQueueEpoch_.load(std::memory_order_acquire);
    const uint8_t packet[] = {0xAA, 0x55, 0x10};

    mock_queue_semaphore_take_result(pdTRUE);
    mock_queue_semaphore_take_result(pdTRUE);
    client.releaseProxyQueues();
    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    TEST_ASSERT_NOT_EQUAL(staleEpoch, client.proxyQueueEpoch_.load(std::memory_order_acquire));

    TEST_ASSERT_FALSE(client.enqueuePhoneCommandForEpoch(packet, sizeof(packet), 0xB2CE, staleEpoch));
    client.forwardToProxyForEpoch(packet, sizeof(packet), 0xB2CE, staleEpoch);
    TEST_ASSERT_EQUAL_UINT32(0, client.phone2v1QueueCount_);
    TEST_ASSERT_EQUAL_UINT32(0, client.proxyQueueCount_);

    TEST_ASSERT_TRUE(client.enqueuePhoneCommand(packet, sizeof(packet), 0xB2CE));
    client.forwardToProxy(packet, sizeof(packet), 0xB2CE);
    TEST_ASSERT_EQUAL_UINT32(1, client.phone2v1QueueCount_);
    TEST_ASSERT_EQUAL_UINT32(1, client.proxyQueueCount_);

    const BleProxyEpochObserverSnapshot observation = client.proxyEpochObserver_.snapshot();
    TEST_ASSERT_EQUAL_UINT32(1, observation.staleV1ToProxyRejections);
    TEST_ASSERT_EQUAL_UINT32(1, observation.staleProxyToV1Rejections);
    TEST_ASSERT_EQUAL_UINT32(1, observation.v1ToProxyAdmissions);
    TEST_ASSERT_EQUAL_UINT32(1, observation.proxyToV1Admissions);
    TEST_ASSERT_FALSE(observation.oldEpochForwarded);
}

void test_missing_phone_queue_records_rejected_epoch_admission() {
    V1BLEClient client;
    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    client.phoneCmdMutex_ = xSemaphoreCreateMutex();
    heap_caps_free(client.phone2v1Queue_);
    client.phone2v1Queue_ = nullptr;
    const uint32_t queueEpoch = client.proxyQueueEpoch_.load(std::memory_order_acquire);

    // Keep admission open for the callback epoch while making a rejected
    // decision observable in the epoch-accounting snapshot.
    client.proxyEpochObserver_.currentEpoch_.store(queueEpoch + 1, std::memory_order_release);
    TEST_ASSERT_TRUE(client.proxyEpochObserver_.accepts(queueEpoch));

    const uint8_t packet[] = {0x11};
    TEST_ASSERT_FALSE(client.enqueuePhoneCommandForEpoch(packet, sizeof(packet), 0xB2CE, queueEpoch));

    const BleProxyEpochObserverSnapshot observation = client.proxyEpochObserver_.snapshot();
    TEST_ASSERT_EQUAL_UINT32(1, observation.staleProxyToV1Rejections);
    TEST_ASSERT_EQUAL_UINT32(0, observation.proxyToV1Admissions);
    TEST_ASSERT_EQUAL_UINT32(1, client.getPhoneCmdDropsInvalid());
}

void test_runtime_disable_closes_epoch_admission_before_queue_release() {
    V1BLEClient client;
    client.proxyEnabled_ = true;
    client.proxyServerInitialized_ = client.initProxyServer("Proxy");
    TEST_ASSERT_TRUE(client.proxyServerInitialized_);
    const uint32_t epoch = client.proxyQueueEpoch_.load(std::memory_order_acquire);
    TEST_ASSERT_TRUE(client.proxyEpochObserver_.accepts(epoch));

    TEST_ASSERT_TRUE(client.setProxyRuntimeEnabled(false));

    TEST_ASSERT_FALSE(client.proxyEpochObserver_.accepts(epoch));
    TEST_ASSERT_FALSE(client.proxyEnabled_);
}

void test_proxy_epoch_snapshot_is_zero_timeout_and_never_returns_partial_busy_state() {
    V1BLEClient client;
    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    client.bleNotifyMutex_ = reinterpret_cast<SemaphoreHandle_t>(0x8101u);
    client.phoneCmdMutex_ = reinterpret_cast<SemaphoreHandle_t>(0x8102u);
    BleProxyEpochQualificationSnapshot snapshot;
    snapshot.proxyQueueCapacity = 99;

    mock_queue_semaphore_take_result(pdFALSE);
    TEST_ASSERT_FALSE(client.trySnapshotProxyEpochQualification(snapshot));
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_semaphore_state.lastTakeTimeout);
    TEST_ASSERT_EQUAL_UINT32(99, snapshot.proxyQueueCapacity);

    mock_queue_semaphore_take_result(pdTRUE);
    mock_queue_semaphore_take_result(pdTRUE);
    TEST_ASSERT_TRUE(client.trySnapshotProxyEpochQualification(snapshot));
    TEST_ASSERT_EQUAL_UINT32(V1BLEClient::PROXY_QUEUE_SIZE, snapshot.proxyQueueCapacity);
    TEST_ASSERT_EQUAL_UINT32(V1BLEClient::PHONE_CMD_QUEUE_SIZE, snapshot.phoneQueueCapacity);
    TEST_ASSERT_EQUAL_UINT32(g_mock_heap_caps_free_size, snapshot.freeInternalBytes);
    TEST_ASSERT_EQUAL_UINT32(g_mock_heap_caps_largest_block, snapshot.largestInternalBlockBytes);
}

#if defined(V1_LINKED_TEST_BLE_PROXY_ALLOC)
void test_linked_source_epoch_gate_rejects_a_callback_crossing_disable_and_reenable() {
    V1BLEClient client;
    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    client.bleNotifyMutex_ = reinterpret_cast<SemaphoreHandle_t>(0x8201u);
    client.phoneCmdMutex_ = reinterpret_cast<SemaphoreHandle_t>(0x8202u);
    const uint32_t oldEpoch = client.proxyQueueEpoch_.load(std::memory_order_acquire);
    std::atomic<bool> callbackEntered{false};
    std::atomic<bool> callbackMayExit{false};

    // This deterministic host barrier proves the linked production observer's
    // ordering only. It is explicitly non-qualifying for the physical race.
    std::thread callback([&]() {
        BleProxyEpochObserver::CallbackLease lease(client.proxyEpochObserver_, BleProxyCallbackDirection::V1ToProxy,
                                                   oldEpoch);
        client.proxyEpochObserver_.noteQueueLockAcquired(BleProxyCallbackDirection::V1ToProxy);
        callbackEntered.store(true, std::memory_order_release);
        while (!callbackMayExit.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        client.proxyEpochObserver_.noteQueueLockReleased(BleProxyCallbackDirection::V1ToProxy);
    });
    while (!callbackEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    mock_queue_semaphore_take_result(pdFALSE);
    client.releaseProxyQueues();
    TEST_ASSERT_TRUE(client.proxyQueueReleasePending_.load(std::memory_order_acquire));
    callbackMayExit.store(true, std::memory_order_release);
    callback.join();
    mock_queue_semaphore_take_result(pdTRUE);
    mock_queue_semaphore_take_result(pdTRUE);
    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    TEST_ASSERT_EQUAL_UINT32(oldEpoch + 1, client.proxyQueueEpoch_.load(std::memory_order_acquire));
    client.proxyEpochObserver_.noteAdmission(BleProxyCallbackDirection::V1ToProxy, oldEpoch, false);

    const BleProxyEpochObserverSnapshot observation = client.proxyEpochObserver_.snapshot();
    TEST_ASSERT_TRUE(observation.activeCallbackObserved);
    TEST_ASSERT_TRUE(observation.releaseOpportunityObserved);
    TEST_ASSERT_EQUAL_UINT32(1, observation.staleV1ToProxyRejections);
    TEST_ASSERT_FALSE(observation.oldEpochForwarded);
    TEST_ASSERT_TRUE(client.proxyEpochObserver_.accepts(oldEpoch + 1));
}
#endif

void test_initProxyServer_full_allocation_failure_disables_proxy_before_server_creation() {
    V1BLEClient client;
    client.proxyEnabled_ = true;
    g_mock_heap_caps_fail_call_mask = 0x0Fu;

    TEST_ASSERT_FALSE(client.initProxyServer("Proxy"));
    TEST_ASSERT_FALSE(client.proxyEnabled_);
    TEST_ASSERT_NULL(client.proxyQueue_);
    TEST_ASSERT_NULL(client.phone2v1Queue_);
    TEST_ASSERT_FALSE(client.proxyQueuesInPsram_);
    TEST_ASSERT_EQUAL_UINT32(4, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_nimble_state.createServerCalls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_nimble_state.createServiceCalls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_nimble_state.createCharacteristicCalls);
}

void test_initProxyServer_partial_failure_frees_partial_allocation_and_resets_state() {
    V1BLEClient client;
    client.proxyEnabled_ = true;
    g_mock_heap_caps_fail_call_mask = (1u << 1) | (1u << 2);

    TEST_ASSERT_FALSE(client.initProxyServer("Proxy"));
    TEST_ASSERT_FALSE(client.proxyEnabled_);
    TEST_ASSERT_NULL(client.proxyQueue_);
    TEST_ASSERT_NULL(client.phone2v1Queue_);
    TEST_ASSERT_FALSE(client.proxyQueuesInPsram_);
    TEST_ASSERT_EQUAL_UINT32(3, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_heap_caps_free_calls);
    TEST_ASSERT_EQUAL_UINT32(0, client.proxyQueueCount_);
    TEST_ASSERT_EQUAL_UINT32(0, client.phone2v1QueueCount_);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_nimble_state.createServerCalls);
}

void test_phone_command_invalid_drop_updates_getters_and_metrics_payload() {
    V1BLEClient client;

    TEST_ASSERT_FALSE(client.enqueuePhoneCommand(nullptr, 0, 0xB2CE));

    assertPhoneCmdDropMetrics(client, 0, 1, 0, 0);
}

void test_phone_command_lock_busy_drop_updates_getters_and_metrics_payload() {
    V1BLEClient client;
    const uint8_t cmd[] = {0x11};

    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    client.phoneCmdMutex_ = nullptr;
    TEST_ASSERT_FALSE(client.enqueuePhoneCommand(cmd, sizeof(cmd), 0xB2CE));

    assertPhoneCmdDropMetrics(client, 0, 0, 0, 1);
}

void test_phone_command_overflow_drops_oldest_and_keeps_newest() {
    V1BLEClient client;

    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    client.phoneCmdMutex_ = xSemaphoreCreateMutex();
    client.connected_ = true;

    for (uint8_t i = 1; i <= V1BLEClient::PHONE_CMD_QUEUE_SIZE + 1; ++i) {
        TEST_ASSERT_TRUE(client.enqueuePhoneCommand(&i, 1, 0xB2CE));
    }

    assertPhoneCmdDropMetrics(client, 1, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(V1BLEClient::PHONE_CMD_QUEUE_SIZE, client.phone2v1QueueCount_);

    while (client.processPhoneCommandQueue() == 1) {
    }

    TEST_ASSERT_EQUAL_UINT32(V1BLEClient::PHONE_CMD_QUEUE_SIZE, g_sentCommandHistory.size());
    TEST_ASSERT_EQUAL_UINT8(2, g_sentCommandHistory.front());
    TEST_ASSERT_EQUAL_UINT8(V1BLEClient::PHONE_CMD_QUEUE_SIZE + 1, g_sentCommandHistory.back());
}

void test_phone_command_ble_failure_updates_getters_and_metrics_payload() {
    V1BLEClient client;
    const uint8_t cmd[] = {0x22};

    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    client.phoneCmdMutex_ = xSemaphoreCreateMutex();
    client.connected_ = true;
    TEST_ASSERT_TRUE(client.enqueuePhoneCommand(cmd, sizeof(cmd), 0xB2CE));

    g_sendCommandResult = SendResult::FAILED;
    TEST_ASSERT_EQUAL_INT(0, client.processPhoneCommandQueue());

    assertPhoneCmdDropMetrics(client, 0, 0, 1, 0);
}

void test_phone_command_drop_metrics_reset_zeroes_all_observable_surfaces() {
    V1BLEClient client;
    const uint8_t cmd[] = {0x33};

    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    client.phoneCmdMutex_ = xSemaphoreCreateMutex();
    client.connected_ = true;

    TEST_ASSERT_FALSE(client.enqueuePhoneCommand(nullptr, 0, 0xB2CE));
    TEST_ASSERT_TRUE(client.enqueuePhoneCommand(cmd, sizeof(cmd), 0xB2CE));
    g_sendCommandResult = SendResult::FAILED;
    TEST_ASSERT_EQUAL_INT(0, client.processPhoneCommandQueue());
    assertPhoneCmdDropMetrics(client, 0, 1, 1, 0);

    perfCounters.reset();
    assertPhoneCmdDropMetrics(client, 0, 0, 0, 0);
}

void test_proxy_app_connect_defers_conn_param_update_until_drained() {
    V1BLEClient client;
    client.proxyEnabled_ = true;
    client.proxyServerInitialized_ = client.initProxyServer("Proxy");
    TEST_ASSERT_TRUE(client.proxyServerInitialized_);
    TEST_ASSERT_NOT_NULL(client.pServer_);
    client.pServer_->setConnectedCount(1);

    V1BLEClient::ProxyServerCallbacks callbacks(&client);
    NimBLEConnInfo connInfo;
    callbacks.onConnect(client.pServer_, connInfo);

    TEST_ASSERT_EQUAL_UINT32(0, g_mock_nimble_state.updateConnParamsCalls);
    TEST_ASSERT_FALSE(client.proxyClientConnected_.load(std::memory_order_relaxed));

    client.drainProxyCallbackEvents();

    TEST_ASSERT_EQUAL_UINT32(1, g_mock_nimble_state.updateConnParamsCalls);
    TEST_ASSERT_TRUE(client.proxyClientConnected_.load(std::memory_order_relaxed));
    TEST_ASSERT_TRUE(client.proxyClientConnectedOnceThisBoot_);
    TEST_ASSERT_EQUAL_UINT32(0, client.proxyFastAdvertisingUntilMs_);
}

void test_proxy_app_disconnect_only_clears_state_when_drained() {
    V1BLEClient client;
    client.proxyEnabled_ = true;
    client.proxyServerInitialized_ = client.initProxyServer("Proxy");
    TEST_ASSERT_TRUE(client.proxyServerInitialized_);
    client.connected_.store(true, std::memory_order_relaxed);
    client.proxyClientConnected_.store(true, std::memory_order_relaxed);
    client.pServer_->setConnectedCount(0);
    mockMillis = 1234;
    const uint32_t startAdvertisingCallsBefore = g_mock_nimble_state.startAdvertisingCalls;

    V1BLEClient::ProxyServerCallbacks callbacks(&client);
    NimBLEConnInfo connInfo;
    callbacks.onDisconnect(client.pServer_, connInfo, 19);

    TEST_ASSERT_TRUE(client.proxyClientConnected_.load(std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_UINT32(startAdvertisingCallsBefore, g_mock_nimble_state.startAdvertisingCalls);
    TEST_ASSERT_EQUAL_UINT32(0, client.proxyAdvertisingStartMs_);

    client.drainProxyCallbackEvents();

    TEST_ASSERT_FALSE(client.proxyClientConnected_.load(std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_UINT32(startAdvertisingCallsBefore, g_mock_nimble_state.startAdvertisingCalls);
    TEST_ASSERT_EQUAL_UINT32(0, client.proxyAdvertisingStartMs_);
    TEST_ASSERT_EQUAL_UINT32(mockMillis + V1BLEClient::PROXY_FAST_RECONNECT_WINDOW_MS,
                             client.proxyFastAdvertisingUntilMs_);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown),
        client.proxyAdvertisingStartReasonCode_);
}

void test_proxy_advertising_downshifts_from_fast_to_slow_cadence() {
    V1BLEClient client;
    client.proxyEnabled_ = true;
    client.proxyServerInitialized_ = client.initProxyServer("Proxy");
    TEST_ASSERT_TRUE(client.proxyServerInitialized_);
    client.connected_.store(true, std::memory_order_relaxed);
    mockMillis = 1000;

    TEST_ASSERT_FALSE(g_mock_nimble_state.advertising);
    client.startProxyAdvertising(
        static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartRetryWindow),
        true);

    TEST_ASSERT_TRUE(g_mock_nimble_state.advertising);
    TEST_ASSERT_TRUE(client.proxyAdvertisingFastCadence_);
    TEST_ASSERT_EQUAL_UINT16(V1BLEClient::PROXY_ADV_FAST_MIN_INTERVAL,
                             g_mock_nimble_state.minInterval);
    TEST_ASSERT_EQUAL_UINT16(V1BLEClient::PROXY_ADV_FAST_MAX_INTERVAL,
                             g_mock_nimble_state.maxInterval);

    const uint32_t startsBeforeDownshift = g_mock_nimble_state.startAdvertisingCalls;
    const uint32_t stopsBeforeDownshift = g_mock_nimble_state.stopAdvertisingCalls;
    mockMillis = client.proxyFastAdvertisingUntilMs_ + 1;
    client.refreshProxyAdvertisingCadence(
        mockMillis,
        static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartRetryWindow));

    TEST_ASSERT_TRUE(g_mock_nimble_state.advertising);
    TEST_ASSERT_FALSE(client.proxyAdvertisingFastCadence_);
    TEST_ASSERT_EQUAL_UINT16(V1BLEClient::PROXY_ADV_SLOW_MIN_INTERVAL,
                             g_mock_nimble_state.minInterval);
    TEST_ASSERT_EQUAL_UINT16(V1BLEClient::PROXY_ADV_SLOW_MAX_INTERVAL,
                             g_mock_nimble_state.maxInterval);
    TEST_ASSERT_EQUAL_UINT32(startsBeforeDownshift + 1, g_mock_nimble_state.startAdvertisingCalls);
    TEST_ASSERT_EQUAL_UINT32(stopsBeforeDownshift + 1, g_mock_nimble_state.stopAdvertisingCalls);
}

void test_proxy_default_name_adopts_v1_advertised_name() {
    V1BLEClient client;
    client.proxyName_ = "V1-Proxy";
    client.proxyEnabled_ = true;
    client.proxyServerInitialized_ = client.initProxyServer("V1-Proxy");
    TEST_ASSERT_TRUE(client.proxyServerInitialized_);

    client.adoptV1AdvertisedNameForProxy("V1C-LE-Test");

    TEST_ASSERT_EQUAL_STRING("V1C-LE-Test", client.proxyName_.c_str());
    TEST_ASSERT_EQUAL_STRING("V1C-LE-Test", g_mock_nimble_state.deviceName.c_str());
    TEST_ASSERT_EQUAL_STRING("V1C-LE-Test", g_mock_nimble_state.advertisementName.c_str());
    TEST_ASSERT_EQUAL_STRING("V1C-LE-Test", g_mock_nimble_state.scanResponseName.c_str());
}

void test_proxy_custom_name_does_not_adopt_v1_advertised_name() {
    V1BLEClient client;
    client.proxyName_ = "Custom-App";
    client.proxyEnabled_ = true;
    client.proxyServerInitialized_ = client.initProxyServer("Custom-App");
    TEST_ASSERT_TRUE(client.proxyServerInitialized_);

    client.adoptV1AdvertisedNameForProxy("V1C-LE-Test");

    TEST_ASSERT_EQUAL_STRING("Custom-App", client.proxyName_.c_str());
    TEST_ASSERT_EQUAL_STRING("Custom-App", g_mock_nimble_state.advertisementName.c_str());
    TEST_ASSERT_EQUAL_STRING("Custom-App", g_mock_nimble_state.scanResponseName.c_str());
}

void test_forward_to_proxy_immediate_queues_until_main_loop_send() {
    V1BLEClient client;
    client.proxyEnabled_ = true;
    client.proxyServerInitialized_ = client.initProxyServer("Proxy");
    TEST_ASSERT_TRUE(client.proxyServerInitialized_);
    client.proxyClientConnected_.store(true, std::memory_order_relaxed);
    client.bleNotifyMutex_ = xSemaphoreCreateMutex();
    const uint8_t data[] = {0xAA, 0x55, 0x10, 0x41, 0x00};

    client.forwardToProxy(data, sizeof(data), 0xB4E0);

    TEST_ASSERT_EQUAL_UINT32(0, g_mock_nimble_state.characteristicNotifyCalls);
    TEST_ASSERT_EQUAL_UINT32(1, client.proxyQueueCount_);

    TEST_ASSERT_EQUAL_INT(1, client.processProxyQueue());

    TEST_ASSERT_EQUAL_UINT32(1, g_mock_nimble_state.characteristicNotifyCalls);
    TEST_ASSERT_EQUAL_UINT32(0, client.proxyQueueCount_);
    TEST_ASSERT_EQUAL_UINT32(1, client.proxyMetrics_.sendCount);
}

void test_proxy_write_callback_records_real_direction_and_admission() {
    V1BLEClient client;
    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    client.connected_.store(true, std::memory_order_relaxed);
    client.phoneCmdMutex_ = xSemaphoreCreateMutex();
    NimBLECharacteristic writeCharacteristic(V1_COMMAND_WRITE_UUID);
    const uint8_t command[] = {0x11, 0x22};
    writeCharacteristic.setValue(command, sizeof(command));
    NimBLEConnInfo connInfo;
    V1BLEClient::ProxyWriteCallbacks callbacks(&client);

    callbacks.onWrite(&writeCharacteristic, connInfo);

    const BleProxyEpochObserverSnapshot observation = client.proxyEpochObserver_.snapshot();
    TEST_ASSERT_EQUAL_UINT32(1, observation.proxyToV1CallbackEntries);
    TEST_ASSERT_EQUAL_UINT32(1, observation.proxyToV1Admissions);
    TEST_ASSERT_EQUAL_UINT32(1, client.phone2v1QueueCount_);
}

void test_notify_callback_preserves_source_characteristic_for_proxy_forwarding() {
    V1BLEClient client;
    client.proxyEnabled_ = true;
    client.proxyServerInitialized_ = client.initProxyServer("Proxy");
    TEST_ASSERT_TRUE(client.proxyServerInitialized_);
    client.proxyClientConnected_.store(true, std::memory_order_relaxed);
    client.bleNotifyMutex_ = xSemaphoreCreateMutex();
    client.acceptClientCallbacks_.store(true, std::memory_order_release);
    client.sessionGeneration_.store(1, std::memory_order_release);
    client.sessionPublicationGate_.open(1);

    NimBLERemoteCharacteristic shortRemote(V1_DISPLAY_DATA_UUID);
    NimBLERemoteCharacteristic longRemote(V1_DISPLAY_DATA_LONG_UUID);
    client.notifyShortChar_.store(&shortRemote, std::memory_order_release);
    client.notifyShortCharId_.store(0xB2CE, std::memory_order_release);
    client.notifyLongChar_.store(&longRemote, std::memory_order_release);
    client.notifyLongCharId_.store(V1_SHORT_UUID_DISPLAY_LONG, std::memory_order_release);

    g_mock_nimble_state.characteristicNotifyCalls = 0;
    g_mock_nimble_state.lastNotifyUuid.clear();
    g_mock_nimble_state.lastNotifyData.clear();

    uint8_t shortData[] = {0xAA, 0x55, 0x10, 0x41, 0x00};
    V1BLEClient::notifyCallback(&shortRemote, shortData, sizeof(shortData), true);

    TEST_ASSERT_EQUAL_UINT32(1, client.proxyQueueCount_);
    TEST_ASSERT_EQUAL_UINT16(0xB2CE, client.proxyQueue_[client.proxyQueueTail_].charUUID);
    TEST_ASSERT_EQUAL_INT(1, client.processProxyQueue());
    TEST_ASSERT_EQUAL_STRING(V1_DISPLAY_DATA_UUID, g_mock_nimble_state.lastNotifyUuid.c_str());
    TEST_ASSERT_EQUAL_UINT32(sizeof(shortData), g_mock_nimble_state.lastNotifyData.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(shortData, g_mock_nimble_state.lastNotifyData.data(), sizeof(shortData));

    uint8_t longData[] = {0xAA, 0x55, 0x10, 0x43, 0x01, 0x02};
    V1BLEClient::notifyCallback(&longRemote, longData, sizeof(longData), true);

    TEST_ASSERT_EQUAL_UINT32(1, client.proxyQueueCount_);
    TEST_ASSERT_EQUAL_UINT16(V1_SHORT_UUID_DISPLAY_LONG,
                             client.proxyQueue_[client.proxyQueueTail_].charUUID);
    TEST_ASSERT_EQUAL_INT(1, client.processProxyQueue());
    TEST_ASSERT_EQUAL_STRING(V1_DISPLAY_DATA_LONG_UUID, g_mock_nimble_state.lastNotifyUuid.c_str());
    TEST_ASSERT_EQUAL_UINT32(sizeof(longData), g_mock_nimble_state.lastNotifyData.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(longData, g_mock_nimble_state.lastNotifyData.data(), sizeof(longData));

    const BleProxyEpochObserverSnapshot observation = client.proxyEpochObserver_.snapshot();
    TEST_ASSERT_EQUAL_UINT32(2, observation.v1ToProxyCallbackEntries);
    TEST_ASSERT_EQUAL_UINT32(2, observation.v1ToProxyAdmissions);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_ble_timing_members_and_constants_use_uint32);
    RUN_TEST(test_allocateProxyQueues_prefers_psram_for_both_buffers);
    RUN_TEST(test_allocateProxyQueues_falls_back_to_internal_when_psram_misses);
    RUN_TEST(test_releaseProxyQueues_defers_when_notify_mutex_is_busy);
    RUN_TEST(test_releaseProxyQueues_defers_when_phone_mutex_is_busy);
    RUN_TEST(test_releaseProxyQueues_retry_frees_after_busy_mutex_clears);
    RUN_TEST(test_releaseProxyQueues_holds_both_mutexes_and_repeated_release_is_harmless);
    RUN_TEST(test_pending_release_gates_queue_producers_and_consumers);
    RUN_TEST(test_allocateProxyQueues_does_not_overwrite_a_pending_release);
    RUN_TEST(test_reallocated_queues_reject_callbacks_from_the_previous_epoch);
    RUN_TEST(test_missing_phone_queue_records_rejected_epoch_admission);
    RUN_TEST(test_runtime_disable_closes_epoch_admission_before_queue_release);
    RUN_TEST(test_proxy_epoch_snapshot_is_zero_timeout_and_never_returns_partial_busy_state);
#if defined(V1_LINKED_TEST_BLE_PROXY_ALLOC)
    RUN_TEST(test_linked_source_epoch_gate_rejects_a_callback_crossing_disable_and_reenable);
#endif
    RUN_TEST(test_initProxyServer_full_allocation_failure_disables_proxy_before_server_creation);
    RUN_TEST(test_initProxyServer_partial_failure_frees_partial_allocation_and_resets_state);
    RUN_TEST(test_phone_command_invalid_drop_updates_getters_and_metrics_payload);
    RUN_TEST(test_phone_command_lock_busy_drop_updates_getters_and_metrics_payload);
    RUN_TEST(test_phone_command_overflow_drops_oldest_and_keeps_newest);
    RUN_TEST(test_phone_command_ble_failure_updates_getters_and_metrics_payload);
    RUN_TEST(test_phone_command_drop_metrics_reset_zeroes_all_observable_surfaces);
    RUN_TEST(test_proxy_app_connect_defers_conn_param_update_until_drained);
    RUN_TEST(test_proxy_app_disconnect_only_clears_state_when_drained);
    RUN_TEST(test_proxy_advertising_downshifts_from_fast_to_slow_cadence);
    RUN_TEST(test_proxy_default_name_adopts_v1_advertised_name);
    RUN_TEST(test_proxy_custom_name_does_not_adopt_v1_advertised_name);
    RUN_TEST(test_forward_to_proxy_immediate_queues_until_main_loop_send);
    RUN_TEST(test_proxy_write_callback_records_real_direction_and_admission);
    RUN_TEST(test_notify_callback_preserves_source_characteristic_for_proxy_forwarding);

    return UNITY_END();
}
