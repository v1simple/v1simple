/**
 * OBD Runtime — Transport Layer
 *
 * Owns the transport slice extracted from obd_runtime_module.cpp.
 *
 * Owns:
 *   - FreeRTOS transport task + queue infrastructure (sObdTransport anon-ns).
 *   - BLE abstraction wrappers over bleClient_.
 *   - Async transport request API (begin/pump/take).
 *   - BLE event queue (ISR-safe enqueue, main-thread drain+apply).
 *   - BLE callbacks from client (onDeviceFound, onBleDisconnect, onBleData).
 *
 * Reads/writes bleBuf_ as part of applyBleEvent — the command TU
 * (obd_runtime_commands.cpp, future carve) drains this buffer during parse.
 */

#include "obd_runtime_module.h"
#include "obd_scan_policy.h"
#include "obd_string_utils.h"
#include "obd_transport_control_dispatch.h"
#if defined(V1SIMPLE_HIL_FAULT_CONTROL)
#include "obd_bsc06_hil_fault_module.h"
#include "obd_bsc13_hil_fault_module.h"
#endif

#include <algorithm>
#include <cstring>

#ifndef UNIT_TEST
extern "C" {
#include "nimble/nimble/host/include/host/ble_att.h"
#include "nimble/nimble/host/include/host/ble_hs.h"
#include "nimble/nimble/include/nimble/ble.h"
}
#endif

#include "../../psram_freertos_alloc.h"
#include "perf_metrics.h"

#ifndef UNIT_TEST
#include "ble_client.h"
#include "obd_ble_client.h"
#endif

// ======================================================================
// FILE-SCOPE HELPERS — copyString for transport code
// ======================================================================

using ObdStringUtils::copyString;

#ifndef UNIT_TEST
constexpr UBaseType_t OBD_TRANSPORT_QUEUE_DEPTH = 1;
constexpr uint32_t OBD_TRANSPORT_STACK_SIZE = 8192;
constexpr UBaseType_t OBD_TRANSPORT_PRIORITY = 1;
constexpr TickType_t OBD_TRANSPORT_RECEIVE_TIMEOUT_TICKS = pdMS_TO_TICKS(20);
constexpr TickType_t OBD_TRANSPORT_CONTROL_RETRY_TICKS = pdMS_TO_TICKS(20);
constexpr size_t OBD_TRANSPORT_ADDR_BUF_LEN = 18;
constexpr size_t OBD_TRANSPORT_CMD_BUF_LEN = 16;

// ======================================================================
// TRANSPORT INFRASTRUCTURE — FreeRTOS task, queues, anonymous namespace state
// ======================================================================

struct ObdTransportRequest {
    ObdTransportOp op = ObdTransportOp::NONE;
    uint32_t requestId = 0;
    uint32_t timeoutMs = 0;
    uint32_t nowMs = 0;
    uint32_t dispatchEpoch = 0;
    uint8_t runtimeStateCode = 0;
    char address[OBD_TRANSPORT_ADDR_BUF_LEN] = {};
    uint8_t addrType = 0;
    bool preferCachedAttributes = false;
    char cmd[OBD_TRANSPORT_CMD_BUF_LEN] = {};
    bool withResponse = false;
    bool deleteBond = false;
};

struct ObdTransportContext {
    ObdBleClient* bleClient = nullptr;
    ObdRuntimeModule* runtime = nullptr;
};

struct ObdTransportRuntime {
    QueueHandle_t requestQueue = nullptr;
    QueueHandle_t controlQueue = nullptr;
    QueueHandle_t resultQueue = nullptr;
    TaskHandle_t task = nullptr;
    PsramQueueAllocation requestQueueAllocation = {};
    PsramQueueAllocation controlQueueAllocation = {};
    PsramQueueAllocation resultQueueAllocation = {};
    bool requestQueueInPsram = false;
    bool controlQueueInPsram = false;
    bool resultQueueInPsram = false;
    bool taskStackInPsram = false;
    ObdTransportRequestEpoch requestEpoch;
    ObdTransportControlDispatch<ObdTransportRequest> controlDispatch;
    ObdTransportContext context = {};
};

ObdTransportRuntime sObdTransport;

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)
uint32_t readObdTransportCancellationEpoch(void*) noexcept {
    return sObdTransport.requestEpoch.snapshot();
}

bool obdTransportLinkDownConfirmed(const uint32_t generation, void* const context) noexcept {
    const auto* transport = static_cast<const ObdTransportContext*>(context);
    return transport != nullptr && transport->bleClient != nullptr &&
           transport->bleClient->linkDownConfirmed(generation);
}
#endif

void publishObdTransportResult(const ObdTransportResult& result) {
    if (sObdTransport.resultQueue) {
        xQueueOverwrite(sObdTransport.resultQueue, &result);
    }
}

bool serviceObdTransportControl(ObdTransportContext* context) {
    if (!context || !context->bleClient) {
        return false;
    }

    return sObdTransport.controlDispatch.service(sObdTransport.controlQueue, sObdTransport.requestQueue,
                                                 *context->bleClient,
                                                 [context](const ObdTransportRequest& control, bool bondDeleteAttempted,
                                                           bool bondDeleted, bool success, bool timedOut) {
                                                     ObdTransportResult result{};
                                                     result.ready = true;
                                                     result.op = ObdTransportOp::DISCONNECT;
                                                     result.requestId = control.requestId;
                                                     result.issuedMs = control.nowMs;
                                                     result.success = success;
                                                     result.timedOut = timedOut;
                                                     result.bondDeleteAttempted = bondDeleteAttempted;
                                                     result.bondDeleted = bondDeleted;
                                                     result.bleError = context->bleClient->getLastBleError();
                                                     publishObdTransportResult(result);
                                                 });
}

void obdTransportTaskEntry(void* param) {
    auto* context = static_cast<ObdTransportContext*>(param);
    if (!context || !context->bleClient || !context->runtime) {
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    while (true) {
        context->bleClient->serviceDeferredLinkState();
        if (serviceObdTransportControl(context)) {
            vTaskDelay(OBD_TRANSPORT_CONTROL_RETRY_TICKS);
            continue;
        }

        ObdTransportRequest request{};
        if (!sObdTransport.requestQueue ||
            xQueueReceive(sObdTransport.requestQueue, &request, OBD_TRANSPORT_RECEIVE_TIMEOUT_TICKS) != pdTRUE) {
            context->bleClient->serviceDeferredLinkState();
            continue;
        }

        // A control request arriving while a data request was queued wins.
        // The local request is intentionally dropped with the old session.
        context->bleClient->serviceDeferredLinkState();
        if (serviceObdTransportControl(context)) {
            vTaskDelay(OBD_TRANSPORT_CONTROL_RETRY_TICKS);
            continue;
        }

        ObdTransportResult result{};
        result.ready = true;
        result.op = request.op;
        result.requestId = request.requestId;
        result.issuedMs = request.nowMs;

        // This CAS is the operation's linearization point. A disconnect that
        // advanced the epoch first drops this stale request; a disconnect
        // that advances it afterward waits for this one in-flight call.
        if (!sObdTransport.requestEpoch.tryClaim(request.dispatchEpoch)) {
            continue;
        }

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)
        const ObdBsc06HilAdmission bsc06Admission{
            request.op == ObdTransportOp::WRITE ? ObdBsc06Operation::Write : ObdBsc06Operation::None,
            context->bleClient->activeLinkGeneration(),
            request.requestId,
            request.dispatchEpoch,
            request.runtimeStateCode,
        };
        if (!obdBsc06HilFaultModule().routeOperation(bsc06Admission, static_cast<uint32_t>(millis()))) {
            result.success = false;
            result.bleError = context->bleClient->getLastBleError();
            sObdTransport.requestEpoch.releaseClaim();
            context->bleClient->serviceDeferredLinkState();
            publishObdTransportResult(result);
            taskYIELD();
            continue;
        }
#endif

        switch (request.op) {
        case ObdTransportOp::CONNECT:
            result.success = context->bleClient->connect(request.address, request.addrType, request.timeoutMs,
                                                         request.preferCachedAttributes);
            result.bleError = context->bleClient->getLastBleError();
            break;
        case ObdTransportOp::DISCONNECT:
            // Production disconnects use the priority control path so their
            // result is callback-confirmed rather than command-accepted.
            result.success = false;
            result.bleError = context->bleClient->getLastBleError();
            break;
        case ObdTransportOp::SECURITY_START:
            result.success = context->bleClient->beginSecurity();
            result.bleError = context->bleClient->getLastBleError();
            result.securityError = context->bleClient->getLastSecurityError();
            break;
        case ObdTransportOp::DISCOVER:
            result.success = context->bleClient->discoverServices();
            result.bleError = context->bleClient->getLastBleError();
            break;
        case ObdTransportOp::SUBSCRIBE:
            result.success = context->bleClient->subscribeNotify([](const uint8_t* data, size_t len) {
                if (sObdTransport.context.runtime) {
                    sObdTransport.context.runtime->onBleData(data, len);
                }
            });
            result.bleError = context->bleClient->getLastBleError();
            break;
        case ObdTransportOp::WRITE:
            result.success = context->bleClient->writeCommand(request.cmd, request.withResponse);
            result.bleError = context->bleClient->getLastBleError();
            break;
        case ObdTransportOp::RSSI_READ:
            result.success = true;
            result.rssi = context->bleClient->getRssi(request.nowMs);
            result.bleError = context->bleClient->getLastBleError();
            break;
        case ObdTransportOp::NONE:
        default:
            result.success = false;
            break;
        }

        sObdTransport.requestEpoch.releaseClaim();
        context->bleClient->serviceDeferredLinkState();
        publishObdTransportResult(result);
        taskYIELD();
    }
}

bool ensureObdTransportRuntime(ObdBleClient* bleClient, ObdRuntimeModule* runtime) {
    if (!bleClient || !runtime) {
        Serial.println("[OBD] ERROR: transport dependencies not provided");
        return false;
    }

    if (!sObdTransport.requestQueue) {
        sObdTransport.requestQueue =
            createQueuePreferPsram(OBD_TRANSPORT_QUEUE_DEPTH, sizeof(ObdTransportRequest),
                                   sObdTransport.requestQueueAllocation, &sObdTransport.requestQueueInPsram);
        if (!sObdTransport.requestQueue) {
            Serial.println("[OBD] ERROR: failed to create transport request queue");
            return false;
        }
    }
    if (!sObdTransport.controlQueue) {
        sObdTransport.controlQueue =
            createQueuePreferPsram(OBD_TRANSPORT_QUEUE_DEPTH, sizeof(ObdTransportRequest),
                                   sObdTransport.controlQueueAllocation, &sObdTransport.controlQueueInPsram);
        if (!sObdTransport.controlQueue) {
            Serial.println("[OBD] ERROR: failed to create transport control queue");
            return false;
        }
    }
    if (!sObdTransport.resultQueue) {
        sObdTransport.resultQueue =
            createQueuePreferPsram(OBD_TRANSPORT_QUEUE_DEPTH, sizeof(ObdTransportResult),
                                   sObdTransport.resultQueueAllocation, &sObdTransport.resultQueueInPsram);
        if (!sObdTransport.resultQueue) {
            Serial.println("[OBD] ERROR: failed to create transport result queue");
            return false;
        }
    }
    if (!sObdTransport.task) {
        sObdTransport.context.bleClient = bleClient;
        sObdTransport.context.runtime = runtime;
#if defined(V1SIMPLE_HIL_FAULT_CONTROL)
        configureObdBsc06HilDeviceRuntime(readObdTransportCancellationEpoch, obdTransportLinkDownConfirmed,
                                          &sObdTransport.context);
        configureObdBsc13HilDeviceRuntime(readObdTransportCancellationEpoch, obdTransportLinkDownConfirmed,
                                          &sObdTransport.context);
#endif

        const BaseType_t rc =
            createTaskPinnedToCoreInternalStack(obdTransportTaskEntry, "ObdTransport", OBD_TRANSPORT_STACK_SIZE,
                                                &sObdTransport.context, OBD_TRANSPORT_PRIORITY, &sObdTransport.task, 0);
        if (rc != pdPASS) {
            Serial.println("[OBD] ERROR: failed to create transport task");
            return false;
        }
    }
    return true;
}

bool ObdRuntimeModule::transportTaskActive() const {
    return sObdTransport.task != nullptr;
}

bool ObdRuntimeModule::transportTaskStackInPsram() const {
    return sObdTransport.taskStackInPsram;
}

uint32_t ObdRuntimeModule::transportStackHighWaterBytes() const {
    TaskHandle_t task = sObdTransport.task;
    if (!task) {
        return 0;
    }
    return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(task));
}
#endif

// ======================================================================
// BLE EVENT QUEUE — enqueue / drain / apply BLE events from callbacks
// ======================================================================

bool ObdRuntimeModule::enqueueBleEvent(const BleEvent& event) {
    BleEvent queuedEvent = event;

    taskENTER_CRITICAL(&bleEventQueueMux_);
    if (bleEventQueueCount_ >= BLE_EVENT_QUEUE_DEPTH) {
        const BleEvent& dropped = bleEventQueue_[bleEventQueueHead_];
        if (queuedEvent.type == BleEventType::DATA && dropped.type == BleEventType::DATA) {
            queuedEvent.overflowed = true;
        }
        bleEventQueueHead_ = (bleEventQueueHead_ + 1) % BLE_EVENT_QUEUE_DEPTH;
        bleEventQueueCount_--;
    }

    const size_t tail = (bleEventQueueHead_ + bleEventQueueCount_) % BLE_EVENT_QUEUE_DEPTH;
    bleEventQueue_[tail] = queuedEvent;
    bleEventQueueCount_++;
    taskEXIT_CRITICAL(&bleEventQueueMux_);
    return true;
}

bool ObdRuntimeModule::popBleEvent(BleEvent& event) {
    taskENTER_CRITICAL(&bleEventQueueMux_);
    if (bleEventQueueCount_ == 0) {
        taskEXIT_CRITICAL(&bleEventQueueMux_);
        return false;
    }

    event = bleEventQueue_[bleEventQueueHead_];
    bleEventQueueHead_ = (bleEventQueueHead_ + 1) % BLE_EVENT_QUEUE_DEPTH;
    bleEventQueueCount_--;
    taskEXIT_CRITICAL(&bleEventQueueMux_);
    return true;
}

void ObdRuntimeModule::applyBleEvent(const BleEvent& event) {
    switch (event.type) {
    case BleEventType::DEVICE_FOUND:
        if (state_ != ObdConnectionState::SCANNING || event.address[0] == '\0' || event.rssi < minRssi_) {
            return;
        }
        copyString(pendingAddress_, sizeof(pendingAddress_), event.address);
        pendingRssi_ = event.rssi;
        pendingAddrType_ = event.addrType;
        pendingDeviceFound_ = true;
        return;

    case BleEventType::DISCONNECT:
        bleDisconnected_ = true;
        bleDisconnectReason_ = event.disconnectReason;
        return;

    case BleEventType::DATA:
        if (state_ != ObdConnectionState::AT_INIT && state_ != ObdConnectionState::POLLING) {
            return;
        }
        {
            const size_t remaining = BLE_BUF_LEN - 1 - bleBufLen_;
            const size_t toCopy = std::min(event.dataLen, remaining);
            if (toCopy > 0) {
                memcpy(bleBuf_ + bleBufLen_, event.data, toCopy);
                bleBufLen_ += toCopy;
                bleBuf_[bleBufLen_] = '\0';
            }
            if (toCopy < event.dataLen || event.overflowed) {
                bleOverflowed_ = true;
            }
            if (event.dataReady) {
                bleDataReady_ = true;
            }
        }
        return;
    }
}

void ObdRuntimeModule::drainBleEventQueue() {
    BleEvent event;
    while (popBleEvent(event)) {
        applyBleEvent(event);
    }
}

void ObdRuntimeModule::clearBleResponseState() {
    bleBufLen_ = 0;
    bleBuf_[0] = '\0';
    bleDataReady_ = false;
    bleOverflowed_ = false;
}

// ======================================================================
// BLE ABSTRACTION WRAPPERS — thin layer over bleClient_
// ======================================================================

bool ObdRuntimeModule::startBleScan() {
#ifndef UNIT_TEST
    return bleClient_ ? bleClient_->startScan(this, minRssi_) : false;
#else
    testStartScanCalls_++;
    return testStartScanResult_;
#endif
}

bool ObdRuntimeModule::connectBle(uint32_t timeoutMs, bool preferCachedAttributes) {
    const char* const address = connectAddress_[0] != '\0' ? connectAddress_ : savedAddress_;
    const uint8_t addrType = connectAddress_[0] != '\0' ? connectAddrType_ : savedAddrType_;
#ifndef UNIT_TEST
    const uint32_t startUs = PERF_TIMESTAMP_US();
    const bool connected = bleClient_->connect(address, addrType, timeoutMs, preferCachedAttributes);
    perfRecordObdConnectCallUs(PERF_TIMESTAMP_US() - startUs);
    return connected;
#else
    (void)address;
    (void)addrType;
    (void)timeoutMs;
    (void)preferCachedAttributes;
    testConnectCalls_++;
    return testConnectResult_;
#endif
}

bool ObdRuntimeModule::isBleConnected() const {
#ifndef UNIT_TEST
    return bleClient_ && bleClient_->isConnected();
#else
    return testBleConnected_;
#endif
}

bool ObdRuntimeModule::beginBleSecurity() {
#ifndef UNIT_TEST
    const uint32_t startUs = PERF_TIMESTAMP_US();
    const bool started = bleClient_->beginSecurity();
    perfRecordObdSecurityStartCallUs(PERF_TIMESTAMP_US() - startUs);
    return started;
#else
    testBeginSecurityCalls_++;
    return testBeginSecurityResult_;
#endif
}

bool ObdRuntimeModule::isBleSecurityReady() const {
#ifndef UNIT_TEST
    return bleClient_->isSecurityReady();
#else
    return testSecurityReady_;
#endif
}

bool ObdRuntimeModule::isBleEncrypted() const {
#ifndef UNIT_TEST
    return bleClient_->isEncrypted();
#else
    return testSecurityEncrypted_;
#endif
}

bool ObdRuntimeModule::isBleBonded() const {
#ifndef UNIT_TEST
    return bleClient_->isBonded();
#else
    return testSecurityBonded_;
#endif
}

bool ObdRuntimeModule::isBleAuthenticated() const {
#ifndef UNIT_TEST
    return bleClient_->isAuthenticated();
#else
    return testSecurityAuthenticated_;
#endif
}

int ObdRuntimeModule::getBleLastError() const {
#ifndef UNIT_TEST
    return bleClient_->getLastBleError();
#else
    return testLastBleError_;
#endif
}

int ObdRuntimeModule::getBleSecurityFailure() const {
#ifndef UNIT_TEST
    return bleClient_->getLastSecurityError();
#else
    return testLastSecurityError_;
#endif
}

bool ObdRuntimeModule::discoverBleServices() {
#ifndef UNIT_TEST
    const uint32_t startUs = PERF_TIMESTAMP_US();
    const bool discovered = bleClient_->discoverServices();
    perfRecordObdDiscoveryCallUs(PERF_TIMESTAMP_US() - startUs);
    return discovered;
#else
    testDiscoverCalls_++;
    return testDiscoverResult_;
#endif
}

bool ObdRuntimeModule::subscribeBleNotifications() {
    // Production subscribe is handled by the transport task in
    // obdTransportTaskEntry() which routes via sObdTransport.context.runtime
    // (proper DI).  This method is only called from the UNIT_TEST fallback
    // path in beginTransportRequest().
#ifdef UNIT_TEST
    return testSubscribeResult_;
#else
    // Should never be reached — production uses the transport queue.
    return false;
#endif
}

bool ObdRuntimeModule::writeBleCommand(const char* cmd, bool withResponse) {
#ifndef UNIT_TEST
    const uint32_t startUs = PERF_TIMESTAMP_US();
    const bool wrote = bleClient_->writeCommand(cmd, withResponse);
    perfRecordObdWriteCallUs(PERF_TIMESTAMP_US() - startUs);
    return wrote;
#else
    testWriteCalls_++;
    copyString(testLastCommand_, sizeof(testLastCommand_), cmd);
    testLastWriteWithResponse_ = withResponse;
    return testWriteResult_;
#endif
}

void ObdRuntimeModule::refreshBleBondBackup() {
#ifndef UNIT_TEST
    ::refreshBleBondBackup();
#else
    testRefreshBondBackupCalls_++;
#endif
}

bool ObdRuntimeModule::queuePendingTransportDisconnect() {
#ifndef UNIT_TEST
    if (!transportDisconnectPending_ || transportDisconnectQueued_) {
        return true;
    }
    if (!ensureObdTransportRuntime(bleClient_, this) || !sObdTransport.controlQueue) {
        Serial.println("[OBD] ERROR: failed to queue transport-owned disconnect");
        return false;
    }

    ObdTransportRequest request{};
    request.op = ObdTransportOp::DISCONNECT;
    request.requestId = pendingDisconnectRequestId_;
    request.nowMs = static_cast<uint32_t>(millis());
    request.deleteBond = pendingDisconnectDeleteBond_;
    request.addrType = pendingDisconnectAddrType_;
    copyString(request.address, sizeof(request.address), pendingDisconnectAddress_);
    if (xQueueOverwrite(sObdTransport.controlQueue, &request) != pdTRUE) {
        Serial.println("[OBD] ERROR: transport disconnect control queue rejected request");
        return false;
    }
    transportDisconnectQueued_ = true;
    return true;
#else
    return true;
#endif
}

bool ObdRuntimeModule::disconnectBle(bool deleteBond) {
    clearTransportRequest();
    readyTransportResult_ = {};
#ifndef UNIT_TEST
    const char* const address = connectAddress_[0] != '\0' ? connectAddress_ : savedAddress_;
    const uint8_t addrType = connectAddress_[0] != '\0' ? connectAddrType_ : savedAddrType_;

    if (transportDisconnectPending_) {
        if (deleteBond && !pendingDisconnectDeleteBond_) {
            copyString(pendingDisconnectAddress_, sizeof(pendingDisconnectAddress_), address);
            pendingDisconnectAddrType_ = addrType;
            if (transportDisconnectQueued_) {
                pendingDisconnectFollowupDeleteBond_ = true;
            } else {
                pendingDisconnectDeleteBond_ = true;
            }
        }
        return queuePendingTransportDisconnect();
    }

    sObdTransport.requestEpoch.cancelQueuedWork();
    transportDisconnectPending_ = true;
    transportDisconnectQueued_ = false;
    pendingDisconnectDeleteBond_ = deleteBond;
    pendingDisconnectFollowupDeleteBond_ = false;
    lastDisconnectSucceeded_ = false;
    pendingDisconnectRequestId_ = ++nextTransportRequestId_;
    copyString(pendingDisconnectAddress_, sizeof(pendingDisconnectAddress_), address);
    pendingDisconnectAddrType_ = addrType;
    return queuePendingTransportDisconnect();
#else
    testDisconnectCalls_++;
    if (deleteBond) {
        testDeleteBondCalls_++;
    }
    testBleConnected_ = false;
    return true;
#endif
}

bool ObdRuntimeModule::disconnectForShutdown(uint32_t timeoutMs) {
    const bool queued = disconnectBle();
#ifndef UNIT_TEST
    if (!queued) {
        return false;
    }
    const uint32_t startedMs = static_cast<uint32_t>(millis());
    while (transportDisconnectPending_ &&
           static_cast<int32_t>(static_cast<uint32_t>(millis()) - startedMs) < static_cast<int32_t>(timeoutMs)) {
        pumpTransportResults();
        delay(1);
    }
    pumpTransportResults();
    return queued && !transportDisconnectPending_ && lastDisconnectSucceeded_;
#else
    (void)queued;
    (void)timeoutMs;
    return true;
#endif
}

void ObdRuntimeModule::stopBleScan() {
#ifndef UNIT_TEST
    bleClient_->stopScan();
#endif
}

int8_t ObdRuntimeModule::readBleRssi(uint32_t nowMs) {
#ifndef UNIT_TEST
    const uint32_t startUs = PERF_TIMESTAMP_US();
    const int8_t rssi = bleClient_->getRssi(nowMs);
    perfRecordObdRssiCallUs(PERF_TIMESTAMP_US() - startUs);
    return rssi;
#else
    (void)nowMs;
    return testRssi_;
#endif
}

void ObdRuntimeModule::clearTransportRequest() {
    transportRequestActive_ = false;
    pendingTransportOp_ = ObdTransportOp::NONE;
    pendingTransportRequestId_ = 0;
    pendingTransportIssuedMs_ = 0;
    pendingTransportTimeoutMs_ = 0;
    pendingTransportTimedOut_ = false;
}

// ======================================================================
// ASYNC TRANSPORT REQUEST API — queueing and result polling
// ======================================================================

bool ObdRuntimeModule::beginTransportRequest(ObdTransportOp op, uint32_t nowMs, uint32_t timeoutMs, const char* cmd,
                                             bool withResponse, bool preferCachedAttributes) {
#ifndef UNIT_TEST
    pumpTransportResults();
#endif
    if (transportRequestActive_ || transportDisconnectPending_) {
        return false;
    }
    readyTransportResult_ = {};

#ifndef UNIT_TEST
    if (!ensureObdTransportRuntime(bleClient_, this) || !sObdTransport.requestQueue) {
        return false;
    }

    ObdTransportRequest request{};
    request.op = op;
    request.requestId = ++nextTransportRequestId_;
    request.timeoutMs = timeoutMs;
    request.nowMs = nowMs;
    request.dispatchEpoch = sObdTransport.requestEpoch.snapshot();
    request.runtimeStateCode = static_cast<uint8_t>(state_);
    const char* const address = connectAddress_[0] != '\0' ? connectAddress_ : savedAddress_;
    const uint8_t addrType = connectAddress_[0] != '\0' ? connectAddrType_ : savedAddrType_;
    request.addrType = addrType;
    request.preferCachedAttributes = preferCachedAttributes;
    request.withResponse = withResponse;
    copyString(request.address, sizeof(request.address), address);
    copyString(request.cmd, sizeof(request.cmd), cmd);

    if (xQueueSend(sObdTransport.requestQueue, &request, 0) != pdTRUE) {
        return false;
    }

    transportRequestActive_ = true;
    pendingTransportOp_ = op;
    pendingTransportRequestId_ = request.requestId;
    pendingTransportIssuedMs_ = nowMs;
    pendingTransportTimeoutMs_ = timeoutMs;
    pendingTransportTimedOut_ = false;
    return true;
#else
    ObdTransportResult result{};
    result.ready = true;
    result.op = op;
    result.requestId = ++nextTransportRequestId_;
    result.issuedMs = nowMs;
    switch (op) {
    case ObdTransportOp::CONNECT:
        result.success = connectBle(timeoutMs, preferCachedAttributes);
        result.bleError = getBleLastError();
        break;
    case ObdTransportOp::DISCONNECT:
        disconnectBle();
        result.success = true;
        result.bleError = getBleLastError();
        break;
    case ObdTransportOp::SECURITY_START:
        result.success = beginBleSecurity();
        result.bleError = getBleLastError();
        result.securityError = getBleSecurityFailure();
        break;
    case ObdTransportOp::DISCOVER:
        result.success = discoverBleServices();
        result.bleError = getBleLastError();
        break;
    case ObdTransportOp::SUBSCRIBE:
        result.success = subscribeBleNotifications();
        result.bleError = getBleLastError();
        break;
    case ObdTransportOp::WRITE:
        result.success = writeBleCommand(cmd, withResponse);
        result.bleError = getBleLastError();
        break;
    case ObdTransportOp::RSSI_READ:
        result.success = true;
        result.rssi = readBleRssi(nowMs);
        result.bleError = getBleLastError();
        break;
    case ObdTransportOp::NONE:
    default:
        result.success = false;
        break;
    }
    readyTransportResult_ = result;
    return true;
#endif
}

void ObdRuntimeModule::pumpTransportResults() {
#ifndef UNIT_TEST
    if (transportDisconnectPending_ && !transportDisconnectQueued_) {
        (void)queuePendingTransportDisconnect();
    }
    if (!sObdTransport.resultQueue) {
        return;
    }

    ObdTransportResult result{};
    while (xQueueReceive(sObdTransport.resultQueue, &result, 0) == pdTRUE) {
        if (result.op == ObdTransportOp::DISCONNECT) {
            if (transportDisconnectPending_ && result.requestId == pendingDisconnectRequestId_) {
                transportDisconnectQueued_ = false;
                if (result.bondDeleteAttempted && !result.bondDeleted) {
                    Serial.printf("[OBD] WARN: transport-owned bond delete failed addr=%s\n",
                                  pendingDisconnectAddress_);
                } else if (result.bondDeleteAttempted) {
                    // Refresh only after transport-owned deletion completes;
                    // doing this when the control was merely queued would
                    // persist the bond that is about to be removed.
                    refreshBleBondBackup();
                }

                lastDisconnectSucceeded_ = result.success && !result.timedOut;
                if (!lastDisconnectSucceeded_) {
                    // Keep the runtime fence closed. The next main-loop pump
                    // requeues at a bounded cadence after the dispatcher has
                    // reported its capped command/confirmation failure.
                    continue;
                }

                if (pendingDisconnectFollowupDeleteBond_) {
                    pendingDisconnectFollowupDeleteBond_ = false;
                    pendingDisconnectDeleteBond_ = true;
                    pendingDisconnectRequestId_ = ++nextTransportRequestId_;
                    lastDisconnectSucceeded_ = false;
                    (void)queuePendingTransportDisconnect();
                    continue;
                }

                transportDisconnectPending_ = false;
                pendingDisconnectDeleteBond_ = false;
                pendingDisconnectRequestId_ = 0;
                pendingDisconnectAddress_[0] = '\0';
                pendingDisconnectAddrType_ = 0;
            }
            continue;
        }
        if (!transportRequestActive_ || result.requestId != pendingTransportRequestId_) {
            continue;
        }
        result.timedOut = pendingTransportTimedOut_;
        readyTransportResult_ = result;
        clearTransportRequest();
    }
#endif
}

bool ObdRuntimeModule::pendingTransportTimedOut(uint32_t nowMs) const {
    return transportRequestActive_ && pendingTransportTimeoutMs_ > 0 &&
           static_cast<int32_t>(nowMs - pendingTransportIssuedMs_) >= static_cast<int32_t>(pendingTransportTimeoutMs_);
}

bool ObdRuntimeModule::takeTransportResult(ObdTransportOp op, ObdTransportResult& result) {
    if (!readyTransportResult_.ready || readyTransportResult_.op != op) {
        return false;
    }
    result = readyTransportResult_;
    readyTransportResult_ = {};
    return true;
}

// ======================================================================
// BLE CALLBACKS FROM CLIENT — device discovery, disconnect, notify
// ======================================================================

void ObdRuntimeModule::onDeviceFound(const char* name, const char* address, int rssi, uint8_t addrType) {
    if (!address || address[0] == '\0') {
        return;
    }
    if (name && strcmp(name, obd::DEVICE_NAME_CX) != 0) {
        return;
    }

    BleEvent event{};
    event.type = BleEventType::DEVICE_FOUND;
    event.rssi = static_cast<int8_t>(rssi);
    event.addrType = addrType;
    copyString(event.address, sizeof(event.address), address);
    enqueueBleEvent(event);
}

void ObdRuntimeModule::onBleDisconnect(int reason) {
    BleEvent event{};
    event.type = BleEventType::DISCONNECT;
    event.disconnectReason = reason;
    enqueueBleEvent(event);
}

void ObdRuntimeModule::onBleData(const uint8_t* data, size_t len) {
    if (!data || len == 0)
        return;

    BleEvent event{};
    event.type = BleEventType::DATA;
    event.dataLen = std::min(len, BLE_BUF_LEN - 1);
    if (event.dataLen > 0) {
        memcpy(event.data, data, event.dataLen);
    }
    event.overflowed = event.dataLen < len;
    event.dataReady = memchr(data, '>', len) != nullptr;
    enqueueBleEvent(event);
}

const char* ObdRuntimeModule::bleReasonName(int reason) {
    switch (reason) {
    case 0:
        return "none";
    case 13:
        return "unacceptable_conn_interval";
    case 520:
        return "supervision_timeout";
    case 534:
        return "local_host_terminated";
#ifndef UNIT_TEST
    case BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING):
        return "pinkey_missing";
    case BLE_HS_HCI_ERR(BLE_ERR_AUTH_FAIL):
        return "auth_fail";
    case BLE_HS_HCI_ERR(BLE_ERR_NO_PAIRING):
        return "no_pairing";
#endif
    default:
        return "unknown";
    }
}

bool ObdRuntimeModule::isSecurityBleError(int error) {
#ifndef UNIT_TEST
    switch (error) {
    case BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN):
    case BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHOR):
    case BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC):
    case BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_KEY_SZ):
    case BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING):
    case BLE_HS_HCI_ERR(BLE_ERR_AUTH_FAIL):
    case BLE_HS_HCI_ERR(BLE_ERR_NO_PAIRING):
    case BLE_HS_HCI_ERR(BLE_ERR_INSUFFICIENT_SEC):
        return true;
    default:
        return false;
    }
#else
    return error != 0;
#endif
}
