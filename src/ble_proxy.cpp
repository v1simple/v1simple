/**
 * BLE Proxy Server Implementation
 * Handles BLE server mode for proxying V1 data to companion apps
 *
 * Extracted from ble_client.cpp — proxy server callbacks, forwarding,
 * phone command queue.
 */

#include "ble_client.h"
#include "ble_internals.h"
#include "config.h"
#include "perf_metrics.h"
#include <esp_heap_caps.h>
#include <algorithm>
#include <cctype>
#include <cstring>

// ============================================================================
// BLE Proxy Server Functions
// ============================================================================

void V1BLEClient::ProxyServerCallbacks::onConnect(NimBLEServer* pServer_, NimBLEConnInfo& connInfo) {
    // NOTE: BLE callback - keep fast
    if (!bleClient) {
        return;
    }

    ProxyCallbackEvent event{};
    event.type = ProxyCallbackEventType::APP_CONNECTED;
    event.connHandle = connInfo.getConnHandle();
    bleClient->enqueueProxyCallbackEvent(event);
}

void V1BLEClient::ProxyServerCallbacks::onDisconnect(NimBLEServer* pServer_, NimBLEConnInfo& connInfo, int reason) {
    // NOTE: BLE callback - keep fast
    if (!bleClient) {
        return;
    }

    ProxyCallbackEvent event{};
    event.type = ProxyCallbackEventType::APP_DISCONNECTED;
    event.reason = reason;
    bleClient->enqueueProxyCallbackEvent(event);
}

void V1BLEClient::deferLastV1Address(const char* addr, const char* advertisedName) {
    if (!addr || addr[0] == '\0') {
        return;
    }
    portENTER_CRITICAL(&pendingAddrMux);
    snprintf(pendingLastV1Address_, sizeof(pendingLastV1Address_), "%s", addr);
    pendingLastV1AddressValid_ = true;
    if (advertisedName && advertisedName[0] != '\0') {
        snprintf(pendingLastV1Name_, sizeof(pendingLastV1Name_), "%s", advertisedName);
        pendingLastV1NameValid_ = true;
    } else {
        pendingLastV1Name_[0] = '\0';
        pendingLastV1NameValid_ = false;
    }
    portEXIT_CRITICAL(&pendingAddrMux);
}

uint32_t V1BLEClient::getPhoneCmdDropsOverflow() const {
    return perfPhoneCmdDropMetricsSnapshot().overflow;
}

uint32_t V1BLEClient::getPhoneCmdDropsInvalid() const {
    return perfPhoneCmdDropMetricsSnapshot().invalid;
}

uint32_t V1BLEClient::getPhoneCmdDropsBleFail() const {
    return perfPhoneCmdDropMetricsSnapshot().bleFail;
}

uint32_t V1BLEClient::getPhoneCmdDropsLockBusy() const {
    return perfPhoneCmdDropMetricsSnapshot().lockBusy;
}

bool V1BLEClient::enqueueProxyCallbackEvent(const ProxyCallbackEvent& event) {
    taskENTER_CRITICAL(&proxyCallbackEventMux_);
    if (proxyCallbackEventQueueCount_ >= PROXY_CALLBACK_EVENT_QUEUE_DEPTH) {
        proxyCallbackEventQueueHead_ =
            (proxyCallbackEventQueueHead_ + 1) % PROXY_CALLBACK_EVENT_QUEUE_DEPTH;
        proxyCallbackEventQueueCount_--;
    }

    const size_t tail =
        (proxyCallbackEventQueueHead_ + proxyCallbackEventQueueCount_) % PROXY_CALLBACK_EVENT_QUEUE_DEPTH;
    proxyCallbackEventQueue_[tail] = event;
    proxyCallbackEventQueueCount_++;
    taskEXIT_CRITICAL(&proxyCallbackEventMux_);
    return true;
}

bool V1BLEClient::popProxyCallbackEvent(ProxyCallbackEvent& event) {
    taskENTER_CRITICAL(&proxyCallbackEventMux_);
    if (proxyCallbackEventQueueCount_ == 0) {
        taskEXIT_CRITICAL(&proxyCallbackEventMux_);
        return false;
    }

    event = proxyCallbackEventQueue_[proxyCallbackEventQueueHead_];
    proxyCallbackEventQueueHead_ =
        (proxyCallbackEventQueueHead_ + 1) % PROXY_CALLBACK_EVENT_QUEUE_DEPTH;
    proxyCallbackEventQueueCount_--;
    taskEXIT_CRITICAL(&proxyCallbackEventMux_);
    return true;
}

void V1BLEClient::clearProxyAdvertisingSchedule() {
    proxyAdvertisingStartMs_ = 0;
    proxyAdvertisingStartReasonCode_ =
        static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
}

void V1BLEClient::armProxyFastAdvertisingWindow(const uint32_t nowMs,
                                                const uint32_t durationMs) {
    proxyFastAdvertisingUntilMs_ = nowMs + durationMs;
}

bool V1BLEClient::proxyFastAdvertisingActive(const uint32_t nowMs) const {
    return proxyFastAdvertisingUntilMs_ != 0 &&
           static_cast<int32_t>(nowMs - proxyFastAdvertisingUntilMs_) < 0;
}

void V1BLEClient::applyProxyAdvertisingCadence(const bool fastCadence) {
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    if (!pAdvertising) {
        return;
    }

    if (fastCadence) {
        pAdvertising->setMinInterval(PROXY_ADV_FAST_MIN_INTERVAL);
        pAdvertising->setMaxInterval(PROXY_ADV_FAST_MAX_INTERVAL);
    } else {
        pAdvertising->setMinInterval(PROXY_ADV_SLOW_MIN_INTERVAL);
        pAdvertising->setMaxInterval(PROXY_ADV_SLOW_MAX_INTERVAL);
    }
    proxyAdvertisingFastCadence_ = fastCadence;
}

void V1BLEClient::refreshProxyAdvertisingCadence(const uint32_t nowMs,
                                                 const uint8_t reasonCode) {
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    if (!pAdvertising || !pAdvertising->isAdvertising()) {
        return;
    }

    const bool fastCadence = proxyFastAdvertisingActive(nowMs);
    if (fastCadence == proxyAdvertisingFastCadence_) {
        return;
    }

    // NimBLE applies advertising interval changes reliably on the next start,
    // so restart in-place when the fast discovery window expires. This is a
    // single short blip, not a user-visible proxy shutdown.
    NimBLEDevice::stopAdvertising();
    applyProxyAdvertisingCadence(fastCadence);
    if (NimBLEDevice::startAdvertising()) {
        perfRecordProxyAdvertisingTransition(true, reasonCode, nowMs);
        Serial.printf("[BLE] Proxy advertising cadence=%s\n",
                      fastCadence ? "fast" : "slow");
    }
}

void V1BLEClient::stopProxyAdvertisingFromMainLoop(uint8_t reasonCode) {
    clearProxyAdvertisingSchedule();

    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (pAdv && pAdv->isAdvertising()) {
        NimBLEDevice::stopAdvertising();
        perfRecordProxyAdvertisingTransition(false, reasonCode, static_cast<uint32_t>(millis()));
    }
}

void V1BLEClient::stopProxyAdvertising() {
    stopProxyAdvertisingFromMainLoop(
        static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopOther));
}

bool V1BLEClient::setProxyRuntimeEnabled(bool enabled, const char* proxyName) {
    if (proxyName && proxyName[0] != '\0') {
        proxyName_ = proxyName;
    }

    if (enabled) {
        if (!proxyServerInitialized_) {
            if (proxyServerInitAttempted_) {
                Serial.println("[BLE] ERROR: Proxy server unavailable; runtime enable requires reboot");
                proxyEnabled_ = false;
                return false;
            }
            proxyEnabled_ = true;
            proxyServerInitialized_ = initProxyServer(proxyName_.c_str());
            proxyServerInitAttempted_ = true;
            if (!proxyServerInitialized_) {
                proxyEnabled_ = false;
                return false;
            }
            return true;
        }

        if (!allocateProxyQueues()) {
            proxyEnabled_ = false;
            return false;
        }
        proxyEnabled_ = true;
        return true;
    }

    clearProxyAdvertisingSchedule();
    stopProxyAdvertisingFromMainLoop(
        static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopOther));
    disconnectProxyPhones();
    proxyClientConnected_ = false;
    proxyEnabled_ = false;
    proxyFastAdvertisingUntilMs_ = 0;
    proxyFastStartWindowArmed_ = false;
    proxyAdvertisingFastCadence_ = true;
    releaseProxyQueues();
    return true;
}

void V1BLEClient::disconnectProxyPhones() {
    clearProxyAdvertisingSchedule();
    proxySuppressedForObdHold_ = true;

    if (!pServer_) {
        proxyClientConnected_ = false;
        return;
    }

    for (uint16_t h : pServer_->getPeerDevices()) {
        pServer_->disconnect(h);
    }
}

bool V1BLEClient::isProxyFullyStopped() const {
    NimBLEAdvertising* pAdv =
        (proxyEnabled_ && proxyServerInitialized_) ? NimBLEDevice::getAdvertising() : nullptr;
    const bool advertisingActive = pAdv && pAdv->isAdvertising();
    const bool phoneConnected =
        (pServer_ && pServer_->getConnectedCount() > 0) ||
        proxyClientConnected_.load(std::memory_order_relaxed);
    return !advertisingActive && !phoneConnected && proxyAdvertisingStartMs_ == 0;
}

void V1BLEClient::configureProxyAdvertisingPayload(const char* deviceName) {
    if (!deviceName || deviceName[0] == '\0' || !pProxyService_) {
        return;
    }

    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    if (!pAdvertising) {
        return;
    }

    NimBLEAdvertisementData advData;
    NimBLEAdvertisementData scanRespData;

    // Advertise as BLE-only to avoid Android attempting BR/EDR pairing.
    advData.setFlags(0x06);
    advData.setCompleteServices(pProxyService_->getUUID());
    advData.setAppearance(0x0C80);

    // Put name in both payloads for Android compatibility.
    advData.setName(deviceName);
    scanRespData.setName(deviceName);

    pAdvertising->setAdvertisementData(advData);
    pAdvertising->setScanResponseData(scanRespData);
}

void V1BLEClient::adoptV1AdvertisedNameForProxy(const char* advertisedName) {
    if (!advertisedName || advertisedName[0] == '\0') {
        return;
    }

    // Treat the shipped default as "auto"; any custom UI-configured proxy name
    // remains authoritative.
    if (proxyName_ != "V1-Proxy") {
        return;
    }

    const char* begin = advertisedName;
    while (*begin != '\0' && isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    const char* end = begin + strlen(begin);
    while (end > begin && isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }

    const size_t trimmedLen = static_cast<size_t>(end - begin);
    if (trimmedLen == 0) {
        return;
    }
    char safeName[33] = {0};
    const size_t copyLen = std::min(trimmedLen, sizeof(safeName) - 1);
    memcpy(safeName, begin, copyLen);

    if (proxyName_ == safeName) {
        return;
    }

    proxyName_ = safeName;
    NimBLEDevice::setDeviceName(proxyName_.c_str());
    if (proxyServerInitialized_) {
        configureProxyAdvertisingPayload(proxyName_.c_str());
    }
    Serial.printf("[BLE] Proxy advertising name auto-set from V1: %s\n",
                  proxyName_.c_str());
}

void V1BLEClient::handleProxyCallbackEvent(const ProxyCallbackEvent& event) {
    switch (event.type) {
        case ProxyCallbackEventType::APP_CONNECTED:
            if (pServer_ && pServer_->getConnectedCount() > 0) {
                // Request Android-compatible connection parameters from the main loop.
                pServer_->updateConnParams(event.connHandle, 12, 36, 0, 400);
            }
            setProxyClientConnected(true);
            proxyFastAdvertisingUntilMs_ = 0;
            proxySuppressedForObdHold_ = false;
            proxyDisconnectRequestedByCoordinator_ = false;
            proxySuppressedResumeReasonCode_ =
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
            perfRecordProxyAdvertisingTransition(
                false,
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopAppConnected),
                static_cast<uint32_t>(millis()));
            return;

        case ProxyCallbackEventType::APP_DISCONNECTED:
            proxyClientConnected_ = false;
            proxyDisconnectRequestedByCoordinator_ = false;
            clearProxyAdvertisingSchedule();
            armProxyFastAdvertisingWindow(static_cast<uint32_t>(millis()),
                                          PROXY_FAST_RECONNECT_WINDOW_MS);
            return;

        case ProxyCallbackEventType::V1_DISCONNECTED:
            proxyClientConnected_ = false;
            proxySuppressedForObdHold_ = false;
            proxyDisconnectRequestedByCoordinator_ = false;
            proxySuppressedResumeReasonCode_ =
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
            stopProxyAdvertisingFromMainLoop(
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopV1Disconnect));
            return;
    }
}

void V1BLEClient::drainProxyCallbackEvents() {
    ProxyCallbackEvent event{};
    while (popProxyCallbackEvent(event)) {
        handleProxyCallbackEvent(event);
    }
}

void V1BLEClient::ProxyWriteCallbacks::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    // Forward commands from app to V1
    // NOTE: This is a BLE callback - avoid blocking operations (Serial, delays, long locks)
    if (!pCharacteristic || !bleClient) {
        return;
    }

    if (!bleClient->connected_.load(std::memory_order_relaxed)) {
        return;
    }

    // Get the raw data pointer and length directly
    NimBLEAttValue attrValue = pCharacteristic->getValue();
    const uint8_t* rawData = attrValue.data();
    size_t rawLen = attrValue.size();

    if (rawLen == 0 || !rawData || rawLen > 32) {
        return;
    }

    // Copy to a safe buffer and forward to V1
    uint16_t sourceChar = shortUuid(pCharacteristic->getUUID());
    uint8_t cmdBuf[32];
    memcpy(cmdBuf, rawData, rawLen);

    // Enqueue for main-loop processing to avoid BLE callback blocking
    bleClient->enqueuePhoneCommand(cmdBuf, rawLen, sourceChar);
}

bool V1BLEClient::allocateProxyQueues() {
    if (proxyQueue_ && phone2v1Queue_) {
        return true;
    }

    releaseProxyQueues();

    bool proxyQueueAllocatedInPsram = false;
    bool phoneQueueAllocatedInPsram = false;

    auto allocateBuffer = [&](size_t count, const char* label, bool& allocatedInPsram) -> ProxyPacket* {
        const size_t bytes = sizeof(ProxyPacket) * count;
        allocatedInPsram = false;

        ProxyPacket* buffer = static_cast<ProxyPacket*>(
            heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
        if (buffer) {
            allocatedInPsram = true;
            memset(buffer, 0, bytes);
            return buffer;
        }

        Serial.printf("[BLE] WARN: %s PSRAM allocation failed (%lu bytes), falling back to internal SRAM\n",
                      label,
                      static_cast<unsigned long>(bytes));
        buffer = static_cast<ProxyPacket*>(
            heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        if (buffer) {
            memset(buffer, 0, bytes);
        }
        return buffer;
    };

    proxyQueue_ = allocateBuffer(PROXY_QUEUE_SIZE, "proxyQueue_", proxyQueueAllocatedInPsram);
    phone2v1Queue_ = allocateBuffer(PHONE_CMD_QUEUE_SIZE, "phone2v1Queue_", phoneQueueAllocatedInPsram);

    if (!proxyQueue_ || !phone2v1Queue_) {
        Serial.println("[BLE] ERROR: Proxy queue allocation failed; disabling proxy");
        releaseProxyQueues();
        proxyEnabled_ = false;
        return false;
    }

    proxyQueuesInPsram_ = proxyQueueAllocatedInPsram && phoneQueueAllocatedInPsram;
    proxyQueueHead_ = 0;
    proxyQueueTail_ = 0;
    proxyQueueCount_ = 0;
    phone2v1QueueHead_ = 0;
    phone2v1QueueTail_ = 0;
    phone2v1QueueCount_ = 0;
    proxyMetrics_.reset();

    Serial.printf("[BLE] Proxy queues allocated (proxy=%s phone=%s)\n",
                  proxyQueueAllocatedInPsram ? "PSRAM" : "INTERNAL",
                  phoneQueueAllocatedInPsram ? "PSRAM" : "INTERNAL");
    return true;
}

void V1BLEClient::releaseProxyQueues() {
    if (proxyQueue_) {
        heap_caps_free(proxyQueue_);
        proxyQueue_ = nullptr;
    }
    if (phone2v1Queue_) {
        heap_caps_free(phone2v1Queue_);
        phone2v1Queue_ = nullptr;
    }

    proxyQueuesInPsram_ = false;
    proxyQueueHead_ = 0;
    proxyQueueTail_ = 0;
    proxyQueueCount_ = 0;
    phone2v1QueueHead_ = 0;
    phone2v1QueueTail_ = 0;
    phone2v1QueueCount_ = 0;
}

bool V1BLEClient::initProxyServer(const char* deviceName) {
    // Proxy server init (name logged in initBLE summary)
    if (!allocateProxyQueues()) {
        return false;
    }

    // Kenny's exact order:
    // 1. Create server (no callbacks yet)
    pServer_ = NimBLEDevice::createServer();

    // 2. Create service
    pProxyService_ = pServer_->createService(V1_SERVICE_UUID);

    // 3. Create ALL 6 characteristics that the V1 exposes (apps expect all of them)
    // Characteristic UUIDs from V1:
    // 92A0B2CE - Display data SHORT (notify) - primary alert data
    // 92A0B4E0 - V1 out LONG (notify)
    // 92A0B6D4 - Client write SHORT (writeNR)
    // 92A0B8D2 - Client write LONG (writeNR)
    // 92A0BCE0 - Notify characteristic
    // 92A0BAD4 - Write with response

    // Primary notify characteristic - display/alert data
    pProxyNotifyChar_ = pProxyService_->createCharacteristic(
        "92A0B2CE-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // V1 out LONG - notify (stores responses like voltage)
    pProxyNotifyLongChar_ = pProxyService_->createCharacteristic(
        "92A0B4E0-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // Client write SHORT - primary command input
    pProxyWriteChar_ = pProxyService_->createCharacteristic(
        "92A0B6D4-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::WRITE_NR
    );

    // Client write LONG
    NimBLECharacteristic* pWriteLong = pProxyService_->createCharacteristic(
        "92A0B8D2-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::WRITE_NR
    );

    // Additional notify characteristic - compatibility stub.
    // The V1 Gen2 exposes this UUID, and some companion apps (V1Driver, JBV1, etc.)
    // verify its presence when enumerating the V1 service. The proxy never notifies
    // on it, but the characteristic must exist in the attribute table for those
    // apps to recognize this device as a V1. Do not remove.
    pProxyService_->createCharacteristic(
        V1_NOTIFY_ALT_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // Alternate write with response
    NimBLECharacteristic* pWriteAlt = pProxyService_->createCharacteristic(
        "92A0BAD4-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );

    // 4. Set characteristic callbacks - all write chars forward to V1
    pProxyWriteCallbacks_.reset(new ProxyWriteCallbacks(this));
    pProxyWriteChar_->setCallbacks(pProxyWriteCallbacks_.get());
    pWriteLong->setCallbacks(pProxyWriteCallbacks_.get());
    pWriteAlt->setCallbacks(pProxyWriteCallbacks_.get());

    // 5. (NimBLE 2.4+) Service is started automatically when advertising starts.
    // pProxyService_->start() is deprecated and must not be called.

    // 6. Set server callbacks (Kenny's order preserved: all characteristics exist before callbacks)
    pProxyServerCallbacks_.reset(new ProxyServerCallbacks(this));
    pServer_->setCallbacks(pProxyServerCallbacks_.get());

    configureProxyAdvertisingPayload(deviceName);
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();

    // Start with fast discovery; runtime downshifts to slow advertising after
    // the user-facing launch window expires.
    applyProxyAdvertisingCadence(true);

    // Start/stop advertising to initialize the stack cleanly before scanning
    pAdvertising->start();
    delay(25);
    NimBLEDevice::stopAdvertising();
    delay(25);

    return true;
}

bool V1BLEClient::isProxyAdvertising() const {
    return proxyEnabled_ && proxyServerInitialized_ &&
           NimBLEDevice::getAdvertising()->isAdvertising();
}

bool V1BLEClient::forceProxyAdvertising(bool enable, uint8_t reasonCode) {
    if (!proxyEnabled_ || !proxyServerInitialized_ || !pServer_) {
        return false;
    }

    const uint8_t startReason = reasonCode == 0
                                    ? static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartDirect)
                                    : reasonCode;
    const uint8_t stopReason = reasonCode == 0
                                   ? static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopOther)
                                   : reasonCode;

    if (enable) {
        if (!connected_.load(std::memory_order_relaxed)) {
            return false;
        }
        // Explicit debug/test control refreshes fast discovery so transition
        // drive flaps do not inherit a stale boot-time cadence.
        armProxyFastAdvertisingWindow(static_cast<uint32_t>(millis()),
                                      PROXY_FAST_START_WINDOW_MS);
        startProxyAdvertising(startReason, true);
        return isProxyAdvertising();
    }

    stopProxyAdvertisingFromMainLoop(stopReason);
    return true;
}

void V1BLEClient::startProxyAdvertising(uint8_t reasonCode, bool ignoreWifiPriority) {
    if (!proxyServerInitialized_ || !pServer_) {
        Serial.println("Cannot start advertising - proxy server not initialized");
        return;
    }

    if (wifiPriorityMode_ && !ignoreWifiPriority) {
        return;
    }

    const uint32_t nowMs = static_cast<uint32_t>(millis());
    if (!proxyFastStartWindowArmed_) {
        proxyFastStartWindowArmed_ = true;
        armProxyFastAdvertisingWindow(nowMs, PROXY_FAST_START_WINDOW_MS);
    }
    applyProxyAdvertisingCadence(proxyFastAdvertisingActive(nowMs));

    const uint32_t startUs = micros();

    // Don't restart if client already connected.
    if (pServer_->getConnectedCount() > 0) {
        perfRecordBleProxyStartUs(micros() - startUs);
        return;
    }

    // Start advertising if not already (simple approach, no task needed)
    if (!NimBLEDevice::getAdvertising()->isAdvertising()) {
        if (NimBLEDevice::startAdvertising()) {
            perfRecordProxyAdvertisingTransition(true, reasonCode, nowMs);
            Serial.printf("Proxy advertising started (%s, cycle=%s cycleMs=%lu)\n",
                          proxyAdvertisingFastCadence_ ? "fast" : "slow",
                          perfConnectionCycleStateName(connectionCycleStateCode_),
                          static_cast<unsigned long>(connectionCycleTimeInStateMs_));
        }
    } else {
        perfRecordProxyAdvertisingTransition(true, reasonCode, static_cast<uint32_t>(millis()));
    }
    perfRecordBleProxyStartUs(micros() - startUs);
}

void V1BLEClient::forwardToProxy(const uint8_t* data, size_t length, uint16_t sourceCharUUID) {
    if (!proxyEnabled_ || !proxyClientConnected_) {
        return;
    }

    // Validate packet size
    if (length == 0 || length > PROXY_PACKET_MAX) {
        return;
    }

    // Protect queue operations from concurrent access (BLE callback vs main loop)
    if (bleNotifyMutex_ && xSemaphoreTake(bleNotifyMutex_, 0) != pdTRUE) {
        // Queue busy – drop to avoid blocking in callback path
        proxyMetrics_.dropCount++;
        return;
    }

    if (!proxyQueue_) {
        proxyMetrics_.dropCount++;
        if (bleNotifyMutex_) xSemaphoreGive(bleNotifyMutex_);
        return;
    }

    // Queue packet for async send (non-blocking)
    // Use simple ring buffer with drop-oldest backpressure
    if (proxyQueueCount_ >= PROXY_QUEUE_SIZE) {
        // Queue full - drop oldest packet
        proxyQueueTail_ = (proxyQueueTail_ + 1) % PROXY_QUEUE_SIZE;
        proxyQueueCount_--;
        proxyMetrics_.dropCount++;
    }

    // Add packet to queue
    ProxyPacket& pkt = proxyQueue_[proxyQueueHead_];
    memcpy(pkt.data, data, length);
    pkt.length = length;
    pkt.charUUID = sourceCharUUID;
    pkt.tsMs = static_cast<uint32_t>(millis());
    proxyQueueHead_ = (proxyQueueHead_ + 1) % PROXY_QUEUE_SIZE;
    proxyQueueCount_++;

    // Track high water mark
    if (proxyQueueCount_ > proxyMetrics_.queueHighWater) {
        proxyMetrics_.queueHighWater = proxyQueueCount_;
    }
    PERF_MAX(proxyQueueHighWater, proxyQueueCount_);

    if (bleNotifyMutex_) {
        xSemaphoreGive(bleNotifyMutex_);
    }
}

int V1BLEClient::processProxyQueue() {
    if (!proxyEnabled_ || !proxyClientConnected_ || proxyQueueCount_ == 0) {
        return 0;
    }

    // HOT PATH: try-lock only, skip if busy (another iteration will process)
    SemaphoreGuard lock(bleNotifyMutex_, 0);
    if (!lock.locked()) {
        return 0;  // Skip this cycle, try again next loop (counter incremented in SemaphoreGuard)
    }

    int sent = 0;

    // Process all queued packets (typically 1-2)
    while (proxyQueueCount_ > 0) {
        ProxyPacket& pkt = proxyQueue_[proxyQueueTail_];
        NimBLECharacteristic* targetChar = nullptr;
        if (pkt.charUUID == V1_SHORT_UUID_DISPLAY_LONG && pProxyNotifyLongChar_) {
            targetChar = pProxyNotifyLongChar_;
        } else if (pProxyNotifyChar_) {
            targetChar = pProxyNotifyChar_;
        }

        if (targetChar) {
            if (targetChar->notify(pkt.data, pkt.length)) {
                proxyMetrics_.sendCount++;
                sent++;
            } else {
                proxyMetrics_.errorCount++;
            }
        }

        proxyQueueTail_ = (proxyQueueTail_ + 1) % PROXY_QUEUE_SIZE;
        proxyQueueCount_--;
    }

    return sent;
}

bool V1BLEClient::enqueuePhoneCommand(const uint8_t* data, size_t length, uint16_t sourceCharUUID) {
    if (!data || length == 0 || length > 32) {
        PERF_INC(phoneCmdDropsInvalid);
        return false;
    }

    if (!phoneCmdMutex_ || xSemaphoreTake(phoneCmdMutex_, 0) != pdTRUE) {
        PERF_INC(phoneCmdDropsLockBusy);
        return false;
    }

    if (!phone2v1Queue_) {
        PERF_INC(phoneCmdDropsInvalid);
        xSemaphoreGive(phoneCmdMutex_);
        return false;
    }

    if (phone2v1QueueCount_ >= PHONE_CMD_QUEUE_SIZE) {
        // Preserve the established queue policy: evict oldest, keep newest.
        phone2v1QueueTail_ = (phone2v1QueueTail_ + 1) % PHONE_CMD_QUEUE_SIZE;
        phone2v1QueueCount_--;
        PERF_INC(phoneCmdDropsOverflow);
    }

    ProxyPacket& pkt = phone2v1Queue_[phone2v1QueueHead_];
    memcpy(pkt.data, data, length);
    pkt.length = length;
    pkt.charUUID = sourceCharUUID;
    phone2v1QueueHead_ = (phone2v1QueueHead_ + 1) % PHONE_CMD_QUEUE_SIZE;
    phone2v1QueueCount_++;
    PERF_MAX(phoneCmdQueueHighWater, phone2v1QueueCount_);

    xSemaphoreGive(phoneCmdMutex_);
    return true;
}

int V1BLEClient::processPhoneCommandQueue() {
    if (!connected_.load(std::memory_order_relaxed)) {
        return 0;
    }

    // Static pending packet: holds command when pacing/lock says "not yet"
    // Ensures no command loss from dequeue-before-send pattern
    static ProxyPacket pendingPkt;
    static uint16_t pendingCharUUID = 0;
    static bool hasPending = false;

    // Clear stale state from previous connection session
    if (phoneCmdPendingClear_) {
        phoneCmdPendingClear_ = false;
        hasPending = false;
        pendingPkt.length = 0;
    }

    ProxyPacket pktCopy;
    uint16_t charUUID = 0;
    bool hasPacket = false;

    // Try pending packet first (from previous pacing/lock deferral)
    if (hasPending) {
        configASSERT(pendingPkt.length <= sizeof(pendingPkt.data));  // Belt-and-suspenders: validated at enqueue
        memcpy(pktCopy.data, pendingPkt.data, pendingPkt.length);
        pktCopy.length = pendingPkt.length;
        charUUID = pendingCharUUID;
        hasPacket = true;
    } else if (phoneCmdMutex_ && xSemaphoreTake(phoneCmdMutex_, 0) == pdTRUE) {
        // Dequeue one packet under lock
        if (phone2v1QueueCount_ > 0) {
            ProxyPacket& pkt = phone2v1Queue_[phone2v1QueueTail_];
            configASSERT(pkt.length <= sizeof(pkt.data));  // Belt-and-suspenders: validated at enqueue
            memcpy(pktCopy.data, pkt.data, pkt.length);
            pktCopy.length = pkt.length;
            charUUID = pkt.charUUID;
            phone2v1QueueTail_ = (phone2v1QueueTail_ + 1) % PHONE_CMD_QUEUE_SIZE;
            phone2v1QueueCount_--;
            hasPacket = true;
        }
        xSemaphoreGive(phoneCmdMutex_);
    }

    if (!hasPacket) {
        return 0;
    }

    // Send outside queue lock - BLE write can take time
    // HOT PATH: try-lock only, skip if busy
    SendResult result = SendResult::FAILED;
    if (bleNotifyMutex_) {
        SemaphoreGuard lock(bleNotifyMutex_, 0);
        if (!lock.locked()) {
            // Mutex busy - store in pending for next iteration (NOT_YET semantics)
            // (counter incremented in SemaphoreGuard)
            memcpy(pendingPkt.data, pktCopy.data, pktCopy.length);
            pendingPkt.length = pktCopy.length;
            pendingCharUUID = charUUID;
            hasPending = true;
            return 0;
        }
        // Snapshot pointer under lock to prevent TOCTOU race with cleanupConnection()
        NimBLERemoteCharacteristic* pCommandCharLongSnapshot = pCommandCharLong_;

        if (charUUID == V1_SHORT_UUID_COMMAND_LONG && pCommandCharLongSnapshot) {
            // Long characteristic write - same transient failure semantics as sendCommand
            if (pCommandCharLongSnapshot->writeValue(pktCopy.data, pktCopy.length, false)) {
                result = SendResult::SENT;
            } else {
                PERF_INC(cmdBleBusy);
                result = SendResult::NOT_YET;  // Transient - retry
            }
        } else {
            result = sendCommandWithResult(pktCopy.data, pktCopy.length);
        }
    } else {
        // Snapshot pointer under bleMutex_ to prevent TOCTOU race with cleanupConnection()
        NimBLERemoteCharacteristic* pCommandCharLongSnapshot = nullptr;
        {
            SemaphoreGuard lock(bleMutex_, 0);
            if (lock.locked()) {
                pCommandCharLongSnapshot = pCommandCharLong_;
            }
        }

        if (charUUID == V1_SHORT_UUID_COMMAND_LONG && pCommandCharLongSnapshot) {
            if (pCommandCharLongSnapshot->writeValue(pktCopy.data, pktCopy.length, false)) {
                result = SendResult::SENT;
            } else {
                PERF_INC(cmdBleBusy);
                result = SendResult::NOT_YET;  // Transient - retry
            }
        } else {
            result = sendCommandWithResult(pktCopy.data, pktCopy.length);
        }
    }

    switch (result) {
        case SendResult::SENT:
            // Successfully sent - clear pending state
            hasPending = false;
            return 1;
        case SendResult::NOT_YET:
            // Pacing: store in pending for next iteration
            memcpy(pendingPkt.data, pktCopy.data, pktCopy.length);
            pendingPkt.length = pktCopy.length;
            pendingCharUUID = charUUID;
            hasPending = true;
            return 0;
        case SendResult::FAILED:
        default:
            // Hard failure: drop packet, clear pending, count error
            hasPending = false;
            PERF_INC(phoneCmdDropsBleFail);
            return 0;
    }
}
