#include "ble_client.h"
#include "ble_internals.h"
#include "config.h"
#include <esp_heap_caps.h>
#include <algorithm>
#include <cctype>
#include <cstring>

void V1BLEClient::ProxyServerCallbacks::onConnect(NimBLEServer* pServer_, NimBLEConnInfo& connInfo) {
    // NimBLE callback: enqueue state and return.
    if (!bleClient) {
        return;
    }

    ProxyCallbackEvent event{};
    event.type = ProxyCallbackEventType::APP_CONNECTED;
    event.connHandle = connInfo.getConnHandle();
    bleClient->enqueueProxyCallbackEvent(event);
}

void V1BLEClient::ProxyServerCallbacks::onDisconnect(NimBLEServer* pServer_, NimBLEConnInfo& connInfo, int reason) {
    // NimBLE callback: enqueue state and return.
    if (!bleClient) {
        return;
    }

    ProxyCallbackEvent event{};
    event.type = ProxyCallbackEventType::APP_DISCONNECTED;
    event.connHandle = connInfo.getConnHandle();
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

bool V1BLEClient::enqueueProxyCallbackEvent(const ProxyCallbackEvent& event) {
    taskENTER_CRITICAL(&proxyCallbackEventMux_);
    ProxyCallbackEvent queuedEvent = event;
    if (event.type == ProxyCallbackEventType::APP_CONNECTED) {
        if (++proxyPhoneGenerationCounter_ == 0) ++proxyPhoneGenerationCounter_;
        proxyPhoneConnectionHandle_ = event.connHandle;
        queuedEvent.phoneSessionGeneration = proxyPhoneGenerationCounter_;
        proxyPhoneSessionGeneration_.store(queuedEvent.phoneSessionGeneration, std::memory_order_release);
    } else if (event.type == ProxyCallbackEventType::APP_DISCONNECTED) {
        if (event.connHandle != proxyPhoneConnectionHandle_) {
            taskEXIT_CRITICAL(&proxyCallbackEventMux_);
            return false;
        }
        queuedEvent.phoneSessionGeneration = proxyPhoneSessionGeneration_.load(std::memory_order_acquire);
        proxyPhoneSessionGeneration_.store(0, std::memory_order_release);
        proxyPhoneConnectionHandle_ = BLE_HS_CONN_HANDLE_NONE;
    }
    if (proxyCallbackEventQueueCount_ >= PROXY_CALLBACK_EVENT_QUEUE_DEPTH) {
        proxyCallbackEventQueueHead_ = (proxyCallbackEventQueueHead_ + 1) % PROXY_CALLBACK_EVENT_QUEUE_DEPTH;
        proxyCallbackEventQueueCount_--;
    }

    const size_t tail =
        (proxyCallbackEventQueueHead_ + proxyCallbackEventQueueCount_) % PROXY_CALLBACK_EVENT_QUEUE_DEPTH;
    proxyCallbackEventQueue_[tail] = queuedEvent;
    proxyCallbackEventQueueCount_++;
    taskEXIT_CRITICAL(&proxyCallbackEventMux_);
    return true;
}

uint32_t V1BLEClient::phoneSessionForConnection(uint16_t connHandle) {
    taskENTER_CRITICAL(&proxyCallbackEventMux_);
    const uint32_t generation = connHandle == proxyPhoneConnectionHandle_
        ? proxyPhoneSessionGeneration_.load(std::memory_order_acquire) : 0;
    taskEXIT_CRITICAL(&proxyCallbackEventMux_);
    return generation;
}

bool V1BLEClient::popProxyCallbackEvent(ProxyCallbackEvent& event) {
    taskENTER_CRITICAL(&proxyCallbackEventMux_);
    if (proxyCallbackEventQueueCount_ == 0) {
        taskEXIT_CRITICAL(&proxyCallbackEventMux_);
        return false;
    }

    event = proxyCallbackEventQueue_[proxyCallbackEventQueueHead_];
    proxyCallbackEventQueueHead_ = (proxyCallbackEventQueueHead_ + 1) % PROXY_CALLBACK_EVENT_QUEUE_DEPTH;
    proxyCallbackEventQueueCount_--;
    taskEXIT_CRITICAL(&proxyCallbackEventMux_);
    return true;
}

void V1BLEClient::clearProxyAdvertisingSchedule() {
    proxyAdvertisingStartMs_ = 0;
}

void V1BLEClient::armProxyFastAdvertisingWindow(const uint32_t nowMs, const uint32_t durationMs) {
    proxyFastAdvertisingUntilMs_ = nowMs + durationMs;
}

bool V1BLEClient::proxyFastAdvertisingActive(const uint32_t nowMs) const {
    return proxyFastAdvertisingUntilMs_ != 0 && static_cast<int32_t>(nowMs - proxyFastAdvertisingUntilMs_) < 0;
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

void V1BLEClient::refreshProxyAdvertisingCadence(const uint32_t nowMs) {
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    if (!pAdvertising || !pAdvertising->isAdvertising()) {
        return;
    }

    const bool fastCadence = proxyFastAdvertisingActive(nowMs);
    if (fastCadence == proxyAdvertisingFastCadence_) {
        return;
    }

    // Interval changes take effect on the next advertising start.
    NimBLEDevice::stopAdvertising();
    applyProxyAdvertisingCadence(fastCadence);
    if (NimBLEDevice::startAdvertising()) {
        Serial.printf("[BLE] Proxy advertising cadence=%s\n", fastCadence ? "fast" : "slow");
    }
}

void V1BLEClient::stopProxyAdvertisingFromMainLoop() {
    clearProxyAdvertisingSchedule();

    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (pAdv && pAdv->isAdvertising()) {
        NimBLEDevice::stopAdvertising();
    }
}

void V1BLEClient::stopProxyAdvertising() {
    stopProxyAdvertisingFromMainLoop();
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

    // Linearize runtime disable before advertising or peer teardown so a
    // callback on the other core cannot enter during the main-loop sequence.
    proxyEpochObserver_.close();
    proxyPhoneSessionGeneration_.store(0, std::memory_order_release);
    clearProxyAdvertisingSchedule();
    stopProxyAdvertisingFromMainLoop();
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
    NimBLEAdvertising* pAdv = (proxyEnabled_ && proxyServerInitialized_) ? NimBLEDevice::getAdvertising() : nullptr;
    const bool advertisingActive = pAdv && pAdv->isAdvertising();
    const bool phoneConnected =
        (pServer_ && pServer_->getConnectedCount() > 0) || proxyClientConnected_.load(std::memory_order_relaxed);
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
    Serial.println("[BLE] Proxy advertising name auto-set from V1");
}

void V1BLEClient::handleProxyCallbackEvent(const ProxyCallbackEvent& event) {
    switch (event.type) {
    case ProxyCallbackEventType::APP_CONNECTED:
        taskENTER_CRITICAL(&proxyCallbackEventMux_);
        if (!acceptsPhoneSession(event.phoneSessionGeneration)) {
            taskEXIT_CRITICAL(&proxyCallbackEventMux_);
            return;
        }
        setProxyClientConnected(true);
        proxyFastAdvertisingUntilMs_ = 0;
        proxySuppressedForObdHold_ = false;
        proxyDisconnectRequestedByCoordinator_ = false;
        taskEXIT_CRITICAL(&proxyCallbackEventMux_);
        if (pServer_ && pServer_->getConnectedCount() > 0) {
            // Request Android-compatible connection parameters from the main loop.
            pServer_->updateConnParams(event.connHandle, 12, 36, 0, 400);
        }
        return;

    case ProxyCallbackEventType::APP_DISCONNECTED:
        taskENTER_CRITICAL(&proxyCallbackEventMux_);
        // A reconnect can arrive before this event is drained. It owns both
        // its new commands and its connection state; an old event cannot clear it.
        if (proxyPhoneSessionGeneration_.load(std::memory_order_acquire) != 0) {
            taskEXIT_CRITICAL(&proxyCallbackEventMux_);
            return;
        }
        proxyClientConnected_ = false;
        proxyDisconnectRequestedByCoordinator_ = false;
        clearProxyAdvertisingSchedule();
        armProxyFastAdvertisingWindow(static_cast<uint32_t>(millis()), PROXY_FAST_RECONNECT_WINDOW_MS);
        taskEXIT_CRITICAL(&proxyCallbackEventMux_);
        return;

    case ProxyCallbackEventType::V1_DISCONNECTED:
        proxyClientConnected_ = false;
        proxySuppressedForObdHold_ = false;
        proxyDisconnectRequestedByCoordinator_ = false;
        stopProxyAdvertisingFromMainLoop();
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
    // NimBLE callback: validate and enqueue the app command without blocking.
    if (!pCharacteristic || !bleClient) {
        return;
    }
    const uint32_t phoneSessionGeneration = bleClient->phoneSessionForConnection(connInfo.getConnHandle());
    if (!phoneSessionGeneration) return;
    const uint32_t sessionGeneration = bleClient->sessionGeneration_.load(std::memory_order_acquire);
    const uint32_t queueEpoch = bleClient->proxyQueueEpoch_.load(std::memory_order_acquire);
    BleProxyEpochObserver::CallbackLease callbackLease(bleClient->proxyEpochObserver_,
                                                       BleProxyCallbackDirection::ProxyToV1, queueEpoch);

    if (!bleClient->connected_.load(std::memory_order_acquire) ||
        !bleClient->sessionPublicationGate_.accepts(sessionGeneration)) {
        return;
    }

    NimBLEAttValue attrValue = pCharacteristic->getValue();
    const uint8_t* rawData = attrValue.data();
    size_t rawLen = attrValue.size();

    if (rawLen == 0 || !rawData || rawLen > 32) {
        return;
    }

    uint16_t sourceChar = shortUuid(pCharacteristic->getUUID());
    uint8_t cmdBuf[32];
    memcpy(cmdBuf, rawData, rawLen);

    bleClient->enqueuePhoneCommandForEpoch(cmdBuf, rawLen, sourceChar, queueEpoch, sessionGeneration,
                                          phoneSessionGeneration);
}

bool V1BLEClient::allocateProxyQueues() {
    if (proxyQueueReleasePending_.load(std::memory_order_acquire) && !tryFinalizeProxyQueueRelease()) {
        return false;
    }

    if (proxyQueue_ && phone2v1Queue_) {
        return true;
    }

    releaseProxyQueues();
    if (proxyQueueReleasePending_.load(std::memory_order_acquire)) {
        return false;
    }

    // Keep callback and main-loop queue users out until both buffers and all
    // indices have been published as one complete allocation.
    proxyQueueReleasePending_.store(true, std::memory_order_release);

    bool proxyQueueAllocatedInPsram = false;
    bool phoneQueueAllocatedInPsram = false;

    auto allocateBuffer = [&](size_t count, const char* label, bool& allocatedInPsram) -> ProxyPacket* {
        const size_t bytes = sizeof(ProxyPacket) * count;
        allocatedInPsram = false;

        ProxyPacket* buffer = static_cast<ProxyPacket*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
        if (buffer) {
            allocatedInPsram = true;
            memset(buffer, 0, bytes);
            return buffer;
        }

        Serial.printf("[BLE] WARN: %s PSRAM allocation failed (%lu bytes), falling back to internal SRAM\n", label,
                      static_cast<unsigned long>(bytes));
        buffer = static_cast<ProxyPacket*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
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
    const uint32_t queueEpoch = proxyQueueEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    proxyQueueReleasePending_.store(false, std::memory_order_release);
    proxyEpochObserver_.open(queueEpoch);

    Serial.printf("[BLE] Proxy queues allocated (proxy=%s phone=%s)\n",
                  proxyQueueAllocatedInPsram ? "PSRAM" : "INTERNAL", phoneQueueAllocatedInPsram ? "PSRAM" : "INTERNAL");
    return true;
}

void V1BLEClient::releaseProxyQueues() {
    // Close admission before attempting either lock. A callback that already
    // passed its first gate rechecks this state after acquiring its queue lock.
    proxyEpochObserver_.close();
    proxyQueueReleasePending_.store(true, std::memory_order_release);
    phoneCmdPendingClear_.store(true, std::memory_order_release);
    tryFinalizeProxyQueueRelease();
}

bool V1BLEClient::tryFinalizeProxyQueueRelease() {
    if (!proxyQueueReleasePending_.load(std::memory_order_acquire)) {
        return true;
    }
    if (proxyEpochObserver_.hasActiveCallbacks()) {
        return false;
    }

    // Repeated teardown after a completed release is intentionally lock-free.
    if (!proxyQueue_ && !phone2v1Queue_) {
        proxyQueuesInPsram_ = false;
        proxyQueueHead_ = 0;
        proxyQueueTail_ = 0;
        proxyQueueCount_ = 0;
        phone2v1QueueHead_ = 0;
        phone2v1QueueTail_ = 0;
        phone2v1QueueCount_ = 0;
        proxyQueueReleasePending_.store(false, std::memory_order_release);
        return true;
    }

    // Sole nested order for proxy storage ownership: notify queue, then phone
    // command queue. Hot callback paths never hold both locks. A failed take
    // leaves both buffers and all indices untouched for the next loop retry.
    bool notifyLocked = false;
    bool phoneLocked = false;
    if (bleNotifyMutex_) {
        if (xSemaphoreTake(bleNotifyMutex_, 0) != pdTRUE) {
            return false;
        }
        notifyLocked = true;
    }
    if (phoneCmdMutex_) {
        if (xSemaphoreTake(phoneCmdMutex_, 0) != pdTRUE) {
            if (notifyLocked) {
                xSemaphoreGive(bleNotifyMutex_);
            }
            return false;
        }
        phoneLocked = true;
    }

    ProxyPacket* const proxyQueue = proxyQueue_;
    ProxyPacket* const phoneQueue = phone2v1Queue_;
    proxyQueue_ = nullptr;
    phone2v1Queue_ = nullptr;

    proxyQueuesInPsram_ = false;
    proxyQueueHead_ = 0;
    proxyQueueTail_ = 0;
    proxyQueueCount_ = 0;
    phone2v1QueueHead_ = 0;
    phone2v1QueueTail_ = 0;
    phone2v1QueueCount_ = 0;

    if (proxyQueue) {
        heap_caps_free(proxyQueue);
    }
    if (phoneQueue) {
        heap_caps_free(phoneQueue);
    }
    proxyQueueReleasePending_.store(false, std::memory_order_release);

    if (phoneLocked) {
        xSemaphoreGive(phoneCmdMutex_);
    }
    if (notifyLocked) {
        xSemaphoreGive(bleNotifyMutex_);
    }
    return true;
}

bool V1BLEClient::initProxyServer(const char* deviceName) {
    if (!allocateProxyQueues()) {
        return false;
    }

    pServer_ = NimBLEDevice::createServer();

    pProxyService_ = pServer_->createService(V1_SERVICE_UUID);

    // Companion apps expect all six V1 characteristics.
    // Characteristic UUIDs from V1:
    // 92A0B2CE - Display data SHORT (notify) - primary alert data
    // 92A0B4E0 - V1 out LONG (notify)
    // 92A0B6D4 - Client write SHORT (writeNR)
    // 92A0B8D2 - Client write LONG (writeNR)
    // 92A0BCE0 - Notify characteristic
    // 92A0BAD4 - Write with response

    // Primary notify characteristic - display/alert data
    pProxyNotifyChar_ = pProxyService_->createCharacteristic("92A0B2CE-9E05-11E2-AA59-F23C91AEC05E",
                                                             NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    // V1 out LONG - notify (stores responses like voltage)
    pProxyNotifyLongChar_ = pProxyService_->createCharacteristic("92A0B4E0-9E05-11E2-AA59-F23C91AEC05E",
                                                                 NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    // Client write SHORT - primary command input
    pProxyWriteChar_ =
        pProxyService_->createCharacteristic("92A0B6D4-9E05-11E2-AA59-F23C91AEC05E", NIMBLE_PROPERTY::WRITE_NR);

    // Client write LONG
    NimBLECharacteristic* pWriteLong =
        pProxyService_->createCharacteristic("92A0B8D2-9E05-11E2-AA59-F23C91AEC05E", NIMBLE_PROPERTY::WRITE_NR);

    // Compatibility-only notify UUID: companion apps require its presence,
    // although the proxy never sends notifications on it.
    pProxyService_->createCharacteristic(V1_NOTIFY_ALT_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    // Alternate write with response
    NimBLECharacteristic* pWriteAlt = pProxyService_->createCharacteristic(
        "92A0BAD4-9E05-11E2-AA59-F23C91AEC05E", NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);

    pProxyWriteCallbacks_.reset(new ProxyWriteCallbacks(this));
    pProxyWriteChar_->setCallbacks(pProxyWriteCallbacks_.get());
    pWriteLong->setCallbacks(pProxyWriteCallbacks_.get());
    pWriteAlt->setCallbacks(pProxyWriteCallbacks_.get());

    // NimBLE starts the service when advertising; do not call deprecated start().

    // Install server callbacks only after all characteristics exist.
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
    return proxyEnabled_ && proxyServerInitialized_ && NimBLEDevice::getAdvertising()->isAdvertising();
}

void V1BLEClient::startProxyAdvertising() {
    if (!proxyServerInitialized_ || !pServer_) {
        Serial.println("Cannot start advertising - proxy server not initialized");
        return;
    }

    const uint32_t nowMs = static_cast<uint32_t>(millis());
    if (!proxyFastStartWindowArmed_) {
        proxyFastStartWindowArmed_ = true;
        armProxyFastAdvertisingWindow(nowMs, PROXY_FAST_START_WINDOW_MS);
    }
    applyProxyAdvertisingCadence(proxyFastAdvertisingActive(nowMs));

    // An active app connection already owns the server.
    if (pServer_->getConnectedCount() > 0) {
        return;
    }

    // Advertising starts inline because this path performs no blocking work.
    if (!NimBLEDevice::getAdvertising()->isAdvertising()) {
        if (NimBLEDevice::startAdvertising()) {
            Serial.printf("Proxy advertising started (%s)\n", proxyAdvertisingFastCadence_ ? "fast" : "slow");
        }
    }
}

void V1BLEClient::forwardToProxy(const uint8_t* data, size_t length, uint16_t sourceCharUUID) {
    forwardToProxyForEpoch(data, length, sourceCharUUID, proxyQueueEpoch_.load(std::memory_order_acquire));
}

void V1BLEClient::forwardToProxyForEpoch(const uint8_t* data, size_t length, uint16_t sourceCharUUID,
                                         uint32_t queueEpoch) {
    if (proxyQueueReleasePending_.load(std::memory_order_acquire) || !proxyEpochObserver_.accepts(queueEpoch) ||
        !proxyClientConnected_) {
        return;
    }

    if (length == 0 || length > PROXY_PACKET_MAX) {
        return;
    }

    // Protect queue operations from concurrent access (BLE callback vs main loop)
    if (bleNotifyMutex_ && xSemaphoreTake(bleNotifyMutex_, 0) != pdTRUE) {
        // Queue busy – drop to avoid blocking in callback path
        return;
    }

    if (proxyQueueReleasePending_.load(std::memory_order_acquire) ||
        proxyQueueEpoch_.load(std::memory_order_acquire) != queueEpoch || !proxyQueue_) {
        if (bleNotifyMutex_)
            xSemaphoreGive(bleNotifyMutex_);
        return;
    }

    // Drop the oldest packet when the ring is full.
    if (proxyQueueCount_ >= PROXY_QUEUE_SIZE) {
        proxyQueueTail_ = (proxyQueueTail_ + 1) % PROXY_QUEUE_SIZE;
        proxyQueueCount_--;
    }

    ProxyPacket& pkt = proxyQueue_[proxyQueueHead_];
    memcpy(pkt.data, data, length);
    pkt.length = length;
    pkt.charUUID = sourceCharUUID;
    pkt.tsMs = static_cast<uint32_t>(millis());
    proxyQueueHead_ = (proxyQueueHead_ + 1) % PROXY_QUEUE_SIZE;
    proxyQueueCount_++;
    if (bleNotifyMutex_) {
        xSemaphoreGive(bleNotifyMutex_);
    }
}

int V1BLEClient::processProxyQueue() {
    if (proxyQueueReleasePending_.load(std::memory_order_acquire) || !proxyEnabled_ || !proxyClientConnected_ ||
        proxyQueueCount_ == 0) {
        return 0;
    }

    // HOT PATH: try-lock only, skip if busy (another iteration will process)
    SemaphoreGuard lock(bleNotifyMutex_, 0);
    if (!lock.locked()) {
        return 0; // Skip this cycle, try again next loop (counter incremented in SemaphoreGuard)
    }
    if (proxyQueueReleasePending_.load(std::memory_order_acquire) || !proxyQueue_) {
        return 0;
    }

    int sent = 0;

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
                sent++;
            }
        }

        proxyQueueTail_ = (proxyQueueTail_ + 1) % PROXY_QUEUE_SIZE;
        proxyQueueCount_--;
    }

    return sent;
}

bool V1BLEClient::enqueuePhoneCommand(const uint8_t* data, size_t length, uint16_t sourceCharUUID) {
    const uint32_t sessionGeneration = sessionGeneration_.load(std::memory_order_acquire);
    return enqueuePhoneCommandForEpoch(data, length, sourceCharUUID, proxyQueueEpoch_.load(std::memory_order_acquire),
                                      sessionGeneration, proxyPhoneSessionGeneration_.load(std::memory_order_acquire));
}

bool V1BLEClient::enqueuePhoneCommandForEpoch(const uint8_t* data, size_t length, uint16_t sourceCharUUID,
                                             uint32_t queueEpoch, uint32_t sessionGeneration, uint32_t phoneSessionGeneration) {
    if (!data || length == 0 || length > 32) {
        return false;
    }
    if (proxyQueueReleasePending_.load(std::memory_order_acquire) || !proxyEpochObserver_.accepts(queueEpoch) ||
        !connected_.load(std::memory_order_acquire) || !sessionPublicationGate_.accepts(sessionGeneration) ||
        !acceptsPhoneSession(phoneSessionGeneration)) {
        return false;
    }

    if (!phoneCmdMutex_ || xSemaphoreTake(phoneCmdMutex_, 0) != pdTRUE) {
        return false;
    }
    if (proxyQueueReleasePending_.load(std::memory_order_acquire) || !proxyEpochObserver_.accepts(queueEpoch) ||
        !connected_.load(std::memory_order_acquire) || !sessionPublicationGate_.accepts(sessionGeneration) ||
        !acceptsPhoneSession(phoneSessionGeneration)) {
        xSemaphoreGive(phoneCmdMutex_);
        return false;
    }
    if (!phone2v1Queue_) {
        xSemaphoreGive(phoneCmdMutex_);
        return false;
    }

    if (phone2v1QueueCount_ >= PHONE_CMD_QUEUE_SIZE) {
        // Preserve the established queue policy: evict oldest, keep newest.
        phone2v1QueueTail_ = (phone2v1QueueTail_ + 1) % PHONE_CMD_QUEUE_SIZE;
        phone2v1QueueCount_--;
    }

    ProxyPacket& pkt = phone2v1Queue_[phone2v1QueueHead_];
    memcpy(pkt.data, data, length);
    pkt.length = length;
    pkt.charUUID = sourceCharUUID;
    pkt.sessionGeneration = sessionGeneration;
    pkt.phoneSessionGeneration = phoneSessionGeneration;
    phone2v1QueueHead_ = (phone2v1QueueHead_ + 1) % PHONE_CMD_QUEUE_SIZE;
    phone2v1QueueCount_++;
    xSemaphoreGive(phoneCmdMutex_);
    return true;
}

int V1BLEClient::processPhoneCommandQueue() {
    CommandScope commandScope(*this, V1CommandOwner::PhoneProxy);
    if (proxyQueueReleasePending_.load(std::memory_order_acquire) || !connected_.load(std::memory_order_relaxed)) {
        return 0;
    }

    // Retain dequeued commands across pacing/lock deferral.
    static ProxyPacket pendingPkt;
    static uint16_t pendingCharUUID = 0;
    static bool hasPending = false;

    // Queue allocation and V1 connection lifetimes are separate. Preserve the
    // callback's V1 generation through retries so neither can cross reconnect.
    if (phoneCmdPendingClear_.exchange(false, std::memory_order_acq_rel) ||
        (hasPending && (!sessionPublicationGate_.accepts(pendingPkt.sessionGeneration) ||
                        !acceptsPhoneSession(pendingPkt.phoneSessionGeneration)))) {
        cancelCommand(V1CommandOwner::PhoneProxy);
        hasPending = false;
        pendingPkt.length = 0;
        phoneLongCommandOwner_ = {};
    }
    if (!sessionPublicationGate_.accepts(phoneLongCommandOwner_.sessionGeneration) ||
        !acceptsPhoneSession(phoneLongCommandOwner_.phoneSessionGeneration)) {
        phoneLongCommandOwner_ = {};
    }

    ProxyPacket pktCopy;
    uint16_t charUUID = 0;
    bool hasPacket = false;

    if (hasPending) {
        configASSERT(pendingPkt.length <= sizeof(pendingPkt.data)); // Belt-and-suspenders: validated at enqueue
        memcpy(pktCopy.data, pendingPkt.data, pendingPkt.length);
        pktCopy.length = pendingPkt.length;
        pktCopy.sessionGeneration = pendingPkt.sessionGeneration;
        pktCopy.phoneSessionGeneration = pendingPkt.phoneSessionGeneration;
        charUUID = pendingCharUUID;
        hasPacket = true;
    } else if (phoneCmdMutex_ && xSemaphoreTake(phoneCmdMutex_, 0) == pdTRUE) {
        // Discard retired sessions under the queue lock before selecting the
        // next command. At most PHONE_CMD_QUEUE_SIZE entries can be visited.
        while (phone2v1QueueCount_ > 0) {
            ProxyPacket& pkt = phone2v1Queue_[phone2v1QueueTail_];
            phone2v1QueueTail_ = (phone2v1QueueTail_ + 1) % PHONE_CMD_QUEUE_SIZE;
            phone2v1QueueCount_--;
            if (!sessionPublicationGate_.accepts(pkt.sessionGeneration) ||
                !acceptsPhoneSession(pkt.phoneSessionGeneration)) {
                continue;
            }
            configASSERT(pkt.length <= sizeof(pkt.data)); // Belt-and-suspenders: validated at enqueue
            memcpy(pktCopy.data, pkt.data, pkt.length);
            pktCopy.length = pkt.length;
            pktCopy.sessionGeneration = pkt.sessionGeneration;
            pktCopy.phoneSessionGeneration = pkt.phoneSessionGeneration;
            charUUID = pkt.charUUID;
            hasPacket = true;
            break;
        }
        xSemaphoreGive(phoneCmdMutex_);
    }

    if (!hasPacket) {
        return 0;
    }
    if (proxyQueueReleasePending_.load(std::memory_order_acquire) ||
        !sessionPublicationGate_.accepts(pktCopy.sessionGeneration) ||
        !acceptsPhoneSession(pktCopy.phoneSessionGeneration)) {
        hasPending = false;
        return 0;
    }

    // Decode only the four ESP header bytes used for pacing; leave every
    // queued payload and its order intact. An indexed header may itself span
    // chunks, and BLE can deliver the chunks out of order.
    PhoneLongCommandOwner longOwner = phoneLongCommandOwner_;
    bool indexed = false;
    bool hasHeader = true;
    uint16_t chunkBit = 0;
    if (charUUID == V1_SHORT_UUID_COMMAND_LONG) {
        const auto isRawFrame = [](const ProxyPacket& packet) {
            // 0xAA is also index 10/count 10. A complete raw ESP envelope
            // distinguishes the compatibility path from that continuation.
            return packet.length >= 6 && packet.data[0] == ESP_PACKET_START &&
                   (packet.data[1] & 0xF0) == 0xD0 && (packet.data[2] & 0xF0) == 0xE0 &&
                   packet.length == 6u + packet.data[4] && packet.data[packet.length - 1] == ESP_PACKET_END;
        };
        if (isRawFrame(pktCopy)) {
            phoneLongCommandOwner_ = {};
        } else {
            indexed = true;
            const uint8_t index = pktCopy.data[0] >> 4;
            const uint8_t count = pktCopy.data[0] & 0x0F;
            bool valid = pktCopy.length >= 2 && pktCopy.length <= 20 && index != 0 && index <= count;
            if (valid) chunkBit = uint16_t{1} << (index - 1);
            if (valid && (longOwner.sessionGeneration != pktCopy.sessionGeneration ||
                          longOwner.phoneSessionGeneration != pktCopy.phoneSessionGeneration ||
                          longOwner.count != count || (longOwner.sentMask & chunkBit) != 0)) {
                longOwner = {};
                hasHeader = false;
                uint8_t starts[4][4] = {};
                uint8_t lengths[4] = {};
                uint16_t seen = 0;
                bool boundary = false;
                bool otherCommandQueued = false;
                const auto inspect = [&](const ProxyPacket& chunk) {
                    const uint8_t part = chunk.data[0] >> 4;
                    const uint16_t bit = part ? uint16_t{1} << (part - 1) : 0;
                    if (chunk.length < 2 || chunk.length > 20 || part == 0 || part > count ||
                        (chunk.data[0] & 0x0F) != count || (seen & bit) != 0 || isRawFrame(chunk)) {
                        boundary = true;
                        return;
                    }
                    seen |= bit;
                    if (part <= 4) {
                        lengths[part - 1] = std::min<size_t>(4, chunk.length - 1);
                        memcpy(starts[part - 1], chunk.data + 1, lengths[part - 1]);
                    }
                };
                inspect(pktCopy);
                if (phoneCmdMutex_ && xSemaphoreTake(phoneCmdMutex_, 0) == pdTRUE) {
                    for (size_t n = 0; n < phone2v1QueueCount_ && !boundary; ++n) {
                        const ProxyPacket& queued = phone2v1Queue_[(phone2v1QueueTail_ + n) % PHONE_CMD_QUEUE_SIZE];
                        if (queued.sessionGeneration != pktCopy.sessionGeneration ||
                            queued.phoneSessionGeneration != pktCopy.phoneSessionGeneration) continue;
                        if (queued.charUUID == V1_SHORT_UUID_COMMAND_LONG) inspect(queued);
                        else otherCommandQueued = true;
                    }
                    xSemaphoreGive(phoneCmdMutex_);
                }
                uint8_t header[4] = {};
                size_t used = 0;
                for (size_t part = 0; part < 4 && lengths[part] != 0 && used < sizeof(header); ++part) {
                    const size_t copy = std::min<size_t>(lengths[part], sizeof(header) - used);
                    memcpy(header + used, starts[part], copy);
                    used += copy;
                }
                hasHeader = used == sizeof(header);
                valid = hasHeader ? header[0] == ESP_PACKET_START
                                  : !boundary && !otherCommandQueued && seen != ((uint16_t{1} << count) - 1);
                if (valid && hasHeader) {
                    longOwner = {pktCopy.sessionGeneration, pktCopy.phoneSessionGeneration, header[1], header[3], count, 0};
                }
            }
            if (!valid) {
                // A malformed/incomplete candidate must not borrow the next
                // command's destination or request ID.
                phoneLongCommandOwner_ = {};
                hasPending = false;
                return 0;
            }
        }
    }
    const bool heldOff = indexed
        ? !hasHeader || isV1RequestHeldOff(longOwner.destination, longOwner.requestId)
        : isV1WriteHeldOff(pktCopy.data, pktCopy.length);

    // Send outside queue lock - BLE write can take time
    // HOT PATH: try-lock only, skip if busy
    SendResult result = SendResult::FAILED;
    if (heldOff && !commandInFlight(V1CommandOwner::PhoneProxy)) {
        result = SendResult::NOT_YET;
    } else if (bleNotifyMutex_) {
        SemaphoreGuard lock(bleNotifyMutex_, 0);
        if (!lock.locked()) {
            // Retry the pending command next loop; preserve NOT_YET semantics.
            memcpy(pendingPkt.data, pktCopy.data, pktCopy.length);
            pendingPkt.length = pktCopy.length;
            pendingPkt.sessionGeneration = pktCopy.sessionGeneration;
            pendingPkt.phoneSessionGeneration = pktCopy.phoneSessionGeneration;
            pendingCharUUID = charUUID;
            hasPending = true;
            return 0;
        }
        if (!sessionPublicationGate_.accepts(pktCopy.sessionGeneration) ||
            !acceptsPhoneSession(pktCopy.phoneSessionGeneration)) {
            hasPending = false;
            return 0;
        }
        NimBLERemoteCharacteristic* characteristic =
            charUUID == V1_SHORT_UUID_COMMAND_LONG && pCommandCharLong_ ? pCommandCharLong_ : pCommandChar_;
        const bool response = characteristic && characteristic == pCommandChar_ && characteristic->canWrite();
        result = sendCharacteristicCommand(characteristic, pktCopy.data, pktCopy.length, response,
                                           pktCopy.phoneSessionGeneration);
    } else {
        // Handles are main-loop-owned. The shared worker gate pins them until
        // completion, including raw B8D2 frames larger than MTU - 3.
        NimBLERemoteCharacteristic* characteristic =
            charUUID == V1_SHORT_UUID_COMMAND_LONG && pCommandCharLong_ ? pCommandCharLong_ : pCommandChar_;
        const bool response = characteristic && characteristic == pCommandChar_ && characteristic->canWrite();
        result = sendCharacteristicCommand(characteristic, pktCopy.data, pktCopy.length, response,
                                           pktCopy.phoneSessionGeneration);
    }

    switch (result) {
    case SendResult::SENT: {
        // Exclude notifications received during the write: they need not
        // reflect the stop yet. Retirement can wait for the next display.
        const uint32_t ingressBoundary = latestV1NotificationIngressSequence();
        if (indexed) {
            longOwner.sentMask |= chunkBit;
            const bool complete = longOwner.sentMask == ((uint16_t{1} << longOwner.count) - 1);
            // Indexed writes bypass sendCommandWithResult. Publish stream
            // state only once the complete command has been forwarded.
            if (complete && longOwner.destination == 0xDA) {
                if (longOwner.requestId == 0x42)
                    alertDataRequestGate_.recordStop(pktCopy.sessionGeneration, pktCopy.phoneSessionGeneration,
                                                     ingressBoundary);
                else if (longOwner.requestId == PACKET_ID_REQ_START_ALERT)
                    alertDataRequestGate_.recordSuccessfulSend(pktCopy.sessionGeneration, static_cast<uint32_t>(millis()));
            }
            phoneLongCommandOwner_ = complete ? PhoneLongCommandOwner{} : longOwner;
        } else if (pktCopy.length == 7 &&
                   pktCopy.data[0] == ESP_PACKET_START && pktCopy.data[1] == 0xDA &&
                   pktCopy.data[2] == 0xE6 && pktCopy.data[4] == 1 &&
                   pktCopy.data[5] == static_cast<uint8_t>(
                       pktCopy.data[0] + pktCopy.data[1] + pktCopy.data[2] +
                       pktCopy.data[3] + pktCopy.data[4]) &&
                   pktCopy.data[6] == ESP_PACKET_END) {
            // Bind a stop to the queued sender, even if phone callbacks retire
            // that sender during the write. Raw long-handle starts need the
            // same bookkeeping as starts sent through the short handle.
            if (pktCopy.data[3] == 0x42)
                alertDataRequestGate_.recordStop(pktCopy.sessionGeneration, pktCopy.phoneSessionGeneration,
                                                 ingressBoundary);
            // Raw START success/rejection is published by the command transport.
        }
        hasPending = false;
        return 1;
    }
    case SendResult::IN_FLIGHT:
    case SendResult::NOT_YET:
        memcpy(pendingPkt.data, pktCopy.data, pktCopy.length);
        pendingPkt.length = pktCopy.length;
        pendingPkt.sessionGeneration = pktCopy.sessionGeneration;
        pendingPkt.phoneSessionGeneration = pktCopy.phoneSessionGeneration;
        pendingCharUUID = charUUID;
        hasPending = true;
        return 0;
    case SendResult::FAILED:
    default:
        if (indexed) phoneLongCommandOwner_ = {};
        hasPending = false;
        return 0;
    }
}
