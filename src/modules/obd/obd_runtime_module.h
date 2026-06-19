#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>

#include "obd_ble_arbitration.h"

enum class ObdConnectionState : uint8_t {
    IDLE = 0,
    WAIT_BOOT = 1,
    SCANNING = 2,
    CONNECTING = 4,
    SECURING = 5,
    DISCOVERING = 6,
    AT_INIT = 7,
    POLLING = 8,
    ERROR_BACKOFF = 9,
    DISCONNECTED = 10,
    ECU_IDLE = 11,
};

enum class ObdCommandKind : uint8_t {
    NONE = 0,
    AT_INIT = 1,
    SANITY = 2,
    SPEED = 3,
};

enum class ObdFailureReason : uint8_t {
    NONE = 0,
    CONNECT_START = 1,
    CONNECT_TIMEOUT = 2,
    DISCOVERY = 3,
    SUBSCRIBE = 4,
    INIT_TIMEOUT = 5,
    INIT_RESPONSE = 6,
    COMMAND_TIMEOUT = 7,
    COMMAND_RESPONSE = 8,
    WRITE = 9,
    BUFFER_OVERFLOW = 10,
    SECURITY_START = 12,
    SECURITY_TIMEOUT = 13,
};

enum class ObdTransportOp : uint8_t {
    NONE = 0,
    CONNECT = 1,
    DISCONNECT = 2,
    SECURITY_START = 3,
    DISCOVER = 4,
    SUBSCRIBE = 5,
    WRITE = 6,
    RSSI_READ = 7,
};

struct ObdTransportResult {
    bool ready = false;
    ObdTransportOp op = ObdTransportOp::NONE;
    uint32_t requestId = 0;
    uint32_t issuedMs = 0;
    bool success = false;
    bool timedOut = false;
    int bleError = 0;
    int securityError = 0;
    int8_t rssi = 0;
};

struct ObdRuntimeStatus {
    bool enabled = false;
    bool connected = false;
    bool securityReady = false;
    bool encrypted = false;
    bool bonded = false;
    bool speedValid = false;
    float speedMph = 0.0f;
    uint32_t speedAgeMs = UINT32_MAX;
    uint32_t speedSampleTsMs = 0;



    int8_t rssi = 0;
    uint8_t connectAttempts = 0;
    uint32_t connectSuccesses = 0;
    uint32_t connectFailures = 0;
    uint32_t securityRepairs = 0;
    bool scanInProgress = false;
    bool manualScanPending = false;
    bool savedAddressValid = false;
    uint8_t initRetries = 0;

    uint32_t pollCount = 0;
    uint32_t pollErrors = 0;
    uint32_t staleSpeedCount = 0;
    uint32_t consecutiveErrors = 0;
    uint32_t totalBytesReceived = 0;
    uint32_t bufferOverflows = 0;

    uint32_t lastConnectStartMs = 0;
    uint32_t lastConnectSuccessMs = 0;
    uint32_t lastFailureMs = 0;
    int lastBleError = 0;
    int lastSecurityError = 0;
    ObdFailureReason lastFailure = ObdFailureReason::NONE;
    ObdCommandKind commandInFlight = ObdCommandKind::NONE;
    ObdConnectionState state = ObdConnectionState::IDLE;
};

struct ObdBleContext {
    bool bootReady = false;
    bool v1Connected = false;
    bool bleScanIdle = false;
    bool v1ConnectBurstSettling = false;
    bool proxyAdvertising = false;
    bool proxyClientConnected = false;
    bool v1ConnectInProgress = false;
    bool obdScanAllowed = true;
    bool obdConnectAllowed = true;
    bool obdRetryAllowed = true;
    uint8_t connectionCycleStateCode = 0;
    uint32_t connectionCycleTimeInStateMs = 0;
};

class ObdRuntimeModule {
public:
    void begin(class ObdBleClient* bleClient,
               bool enabled,
               const char* savedAddress,
               uint8_t savedAddrType,
               int8_t minRssi);
    void update(uint32_t nowMs, const ObdBleContext& bootReadyContext);
    void update(uint32_t nowMs, bool bootReady, bool v1Connected, bool bleScanIdle) {
        update(nowMs, ObdBleContext{bootReady, v1Connected, bleScanIdle});
    }
    ObdRuntimeStatus snapshot(uint32_t nowMs) const;
    ObdConnectionState getState() const { return state_; }

    void setEnabled(bool enabled);
    void setMinRssi(int8_t minRssi);
    bool isEnabled() const { return enabled_; }
#ifndef UNIT_TEST
    bool transportTaskActive() const;
    bool transportTaskStackInPsram() const;
    uint32_t transportStackHighWaterBytes() const;
#endif

    bool getFreshSpeed(uint32_t nowMs, float& speedMphOut, uint32_t& tsMsOut) const;

    const char* getSavedAddress() const { return savedAddress_; }
    uint8_t getSavedAddrType() const { return savedAddrType_; }

    bool startScan();
    void stopActiveScan();
    bool requestManualPairScan(uint32_t nowMs);
    void cancelPendingConnect();
    void forgetDevice();
    bool isScanStopped() const;
    bool isConnectIdle() const;

    void onDeviceFound(const char* name, const char* address, int rssi, uint8_t addrType = 0);
    void onBleDisconnect(int reason = 0);
    void onBleData(const uint8_t* data, size_t len);

#ifdef UNIT_TEST
    void injectSpeedForTest(float speedMph, uint32_t timestampMs);
    void forceStateForTest(ObdConnectionState state, uint32_t enteredMs);
    void setConsecutiveErrorsForTest(uint32_t errors) { consecutiveErrors_ = errors; }
    void setBackoffCyclesForTest(uint32_t cycles) { backoffCycles_ = cycles; }
    void setV1WasConnectedAtEcuIdleForTest(bool v) { v1WasConnectedAtEcuIdle_ = v; }
    void setConsecutiveSpeedSamplesForTest(uint32_t samples) { consecutiveSpeedSamples_ = samples; }
    void setLastFailureForTest(ObdFailureReason reason) { lastFailure_ = reason; }
    ObdCommandKind getActiveCommandKindForTest() const;
    uint32_t getStartScanCallCountForTest() const { return testStartScanCalls_; }
    uint32_t getConnectCallCountForTest() const { return testConnectCalls_; }
    uint32_t getDiscoverCallCountForTest() const { return testDiscoverCalls_; }
    uint32_t getDisconnectCallCountForTest() const { return testDisconnectCalls_; }
    uint32_t getWriteCallCountForTest() const { return testWriteCalls_; }
    uint32_t getBeginSecurityCallCountForTest() const { return testBeginSecurityCalls_; }
    uint32_t getDeleteBondCallCountForTest() const { return testDeleteBondCalls_; }
    uint32_t getRefreshBondBackupCallCountForTest() const { return testRefreshBondBackupCalls_; }
    const char* getRepairedBondAddressForTest() const { return repairedBondAddress_; }
    const char* getConnectAddressForTest() const { return connectAddress_; }
    const char* getLastCommandForTest() const { return testLastCommand_; }
    bool getLastWriteWithResponseForTest() const { return testLastWriteWithResponse_; }
    void setTestStartScanResult(bool result) { testStartScanResult_ = result; }
    void setTestConnectResult(bool result) { testConnectResult_ = result; }
    void setTestBleConnected(bool connected) { testBleConnected_ = connected; }
    void setTestDiscoverResult(bool result) { testDiscoverResult_ = result; }
    void setTestSubscribeResult(bool result) { testSubscribeResult_ = result; }
    void setTestWriteResult(bool result) { testWriteResult_ = result; }
    void setTestRssi(int8_t rssi) { testRssi_ = rssi; }
    void setTestBeginSecurityResult(bool result) { testBeginSecurityResult_ = result; }
    void setTestSecurityReady(bool ready) { testSecurityReady_ = ready; }
    void setTestSecurityEncrypted(bool encrypted) { testSecurityEncrypted_ = encrypted; }
    void setTestSecurityBonded(bool bonded) { testSecurityBonded_ = bonded; }
    void setTestSecurityAuthenticated(bool authenticated) { testSecurityAuthenticated_ = authenticated; }
    void setTestLastBleError(int error) { testLastBleError_ = error; }
    void setTestLastSecurityError(int error) { testLastSecurityError_ = error; }
    static const char* bleReasonNameForTest(int reason) { return bleReasonName(reason); }
    void transitionToPollingForTest(uint32_t nowMs);
    ObdBleArbitrationRequest getBleArbitrationRequest() const;
#endif

private:
    static constexpr size_t ADDR_BUF_LEN = 18;
    static constexpr size_t CMD_BUF_LEN = 16;
    static constexpr size_t BLE_BUF_LEN = 256;

    enum class ParserKind : uint8_t {
        NONE = 0,
        AT_TEXT = 1,
        SIMPLE = 2,
        VIN = 3,
    };

    struct ActiveObdCommand {
        bool active = false;
        ObdCommandKind kind = ObdCommandKind::NONE;
        ParserKind parser = ParserKind::NONE;
        char tx[CMD_BUF_LEN] = {};
        uint8_t expectedService = 0;
        uint8_t expectedPid = 0;
        uint16_t expectedDid = 0;
        uint32_t timeoutMs = 0;
        uint8_t retriesRemaining = 0;
        uint32_t sentMs = 0;
        bool writeWithResponse = true;
        bool alternateWriteModeTried = false;
    };

    void resetForBegin();
    void transitionTo(ObdConnectionState newState, uint32_t nowMs);
    void clearBleResponseState();
    void clearSpeedState();
    void resetPollingSchedule(uint32_t nowMs);
    void resetInitState(bool preferWarmInit);
    void resetCommandState();

    void markFailure(ObdFailureReason reason, uint32_t nowMs);
    void handleConnectFailure(uint32_t nowMs, ObdFailureReason reason, int bleReason = 0);
    void handlePollingError(uint32_t nowMs, bool disconnectBleNow, ObdFailureReason reason);
    void handleCommandFailure(uint32_t nowMs, ObdFailureReason reason, bool disconnectBleNow);
    static bool shouldDisconnectAfterPollingError(ObdFailureReason reason);
    void setSavedAddressFromBuffer(const char* address);
    void setConnectTarget(const char* address, uint8_t addrType, bool fromManualCandidate);
    void setConnectTargetFromSaved();
    void clearConnectTarget();
    void clearManualScanState();
    void commitManualScanCandidate();

    bool startBleScan();
    bool connectBle(uint32_t timeoutMs, bool preferCachedAttributes);
    bool isBleConnected() const;
    bool beginBleSecurity();
    bool isBleSecurityReady() const;
    bool isBleEncrypted() const;
    bool isBleBonded() const;
    bool isBleAuthenticated() const;
    int getBleLastError() const;
    int getBleSecurityFailure() const;
    bool discoverBleServices();
    bool subscribeBleNotifications();
    bool writeBleCommand(const char* cmd, bool withResponse);
    bool deleteBleBond();
    void refreshBleBondBackup();
    void disconnectBle();
    void stopBleScan();
    int8_t readBleRssi(uint32_t nowMs);

    bool validateAtResponse(const char* command, const char* response, size_t len) const;
    bool startCommand(ObdCommandKind kind,
                      ParserKind parser,
                      const char* tx,
                      uint8_t expectedService,
                      uint8_t expectedPid,
                      uint16_t expectedDid,
                      uint32_t timeoutMs,
                      uint8_t retries,
                      uint32_t nowMs);
    bool retryActiveCommand(uint32_t nowMs);
    bool retryActiveCommandWithAlternateWriteMode(uint32_t nowMs);
    void completeActiveCommand();
    void handleAtInitResponse(uint32_t nowMs);
    void handlePollingResponse(uint32_t nowMs);
    void updateSecuring(uint32_t nowMs);
    void updateAtInit(uint32_t nowMs);
    void updatePolling(uint32_t nowMs);

    bool isSpeedFresh(uint32_t nowMs) const;
    bool speedDue(uint32_t nowMs) const;
    bool sendNextPollingCommand(uint32_t nowMs);
    bool startSpeedCommand(uint32_t nowMs);

    bool handleSpeedResponse(uint32_t nowMs);
    bool validateSimpleResponse(uint8_t expectedService,
                                uint8_t expectedPid,
                                const char* response,
                                size_t len) const;

    static const char* bleReasonName(int reason);
    static bool isSecurityBleError(int error);

    bool canAutoHealBond() const;
    bool autoHealBondIfAllowed(uint32_t nowMs, const char* context);
    void pumpTransportResults();
    bool beginTransportRequest(ObdTransportOp op,
                               uint32_t nowMs,
                               uint32_t timeoutMs,
                               const char* cmd = nullptr,
                               bool withResponse = false,
                               bool preferCachedAttributes = false);
    bool pendingTransportTimedOut(uint32_t nowMs) const;
    bool takeTransportResult(ObdTransportOp op, ObdTransportResult& result);
    void clearTransportRequest();
    void clearBleEventQueue();
    void drainBleEventQueue();

    enum class BleEventType : uint8_t {
        DEVICE_FOUND = 0,
        DISCONNECT = 1,
        DATA = 2,
    };

    struct BleEvent {
        BleEventType type = BleEventType::DATA;
        size_t dataLen = 0;
        int disconnectReason = 0;
        int8_t rssi = 0;
        uint8_t addrType = 0;
        bool dataReady = false;
        bool overflowed = false;
        char address[ADDR_BUF_LEN] = {};
        uint8_t data[BLE_BUF_LEN - 1] = {};
    };

    bool enqueueBleEvent(const BleEvent& event);
    bool popBleEvent(BleEvent& event);
    void applyBleEvent(const BleEvent& event);

    class ObdBleClient* bleClient_ = nullptr;

    bool enabled_ = false;
    ObdConnectionState state_ = ObdConnectionState::IDLE;
    uint32_t stateEnteredMs_ = 0;
    uint32_t bootReadyMs_ = 0;
    bool stateEntryPending_ = false;
    uint8_t connectionCycleStateCode_ = 0;
    uint32_t connectionCycleTimeInStateMs_ = 0;
    bool lastProxyAdvertising_ = false;
    bool lastProxyClientConnected_ = false;
    bool lastObdRetryAllowed_ = true;

    float speedMph_ = 0.0f;
    uint32_t speedSampleTsMs_ = 0;
    bool speedValid_ = false;



    char savedAddress_[ADDR_BUF_LEN] = {};
    char connectAddress_[ADDR_BUF_LEN] = {};
    char manualCandidateAddress_[ADDR_BUF_LEN] = {};
    char pendingAddress_[ADDR_BUF_LEN] = {};
    int8_t minRssi_ = -80;
    int8_t rssi_ = 0;
    int8_t pendingRssi_ = 0;
    uint8_t pendingAddrType_ = 0;
    uint8_t savedAddrType_ = 0;
    uint8_t connectAddrType_ = 0;
    uint8_t manualCandidateAddrType_ = 0;
    bool pendingDeviceFound_ = false;
    bool scanRequested_ = false;
    bool manualScanPending_ = false;
    bool manualScanPreemptProxy_ = false;
    bool manualCandidateValid_ = false;
    bool connectTargetFromManualCandidate_ = false;
    bool preferWarmReconnect_ = false;
    bool warmInitPreferred_ = false;
    bool coldInitFallbackUsed_ = false;
    bool preferWriteWithResponse_ = true;

    uint8_t connectAttempts_ = 0;
    uint32_t connectSuccesses_ = 0;
    uint32_t connectFailures_ = 0;
    uint32_t pollCount_ = 0;
    uint32_t pollErrors_ = 0;
    uint32_t staleSpeedCount_ = 0;
    uint32_t consecutiveErrors_ = 0;
    uint32_t backoffCycles_ = 0;
    bool v1WasConnectedAtEcuIdle_ = false;
    uint32_t totalBytesReceived_ = 0;
    uint32_t lastRssiMs_ = 0;
    uint32_t bufferOverflowCount_ = 0;
    uint8_t initRetries_ = 0;
    uint32_t consecutiveSpeedSamples_ = 0;
    uint32_t securityRepairs_ = 0;
    uint32_t lastConnectStartMs_ = 0;
    uint32_t lastConnectSuccessMs_ = 0;
    uint32_t lastFailureMs_ = 0;
    ObdFailureReason lastFailure_ = ObdFailureReason::NONE;
    char repairedBondAddress_[ADDR_BUF_LEN] = {};

    uint32_t nextSpeedDueMs_ = 0;
    uint8_t initIndex_ = 0;

    ActiveObdCommand activeCommand_ = {};
    bool transportRequestActive_ = false;
    ObdTransportOp pendingTransportOp_ = ObdTransportOp::NONE;
    uint32_t nextTransportRequestId_ = 0;
    uint32_t pendingTransportRequestId_ = 0;
    uint32_t pendingTransportIssuedMs_ = 0;
    uint32_t pendingTransportTimeoutMs_ = 0;
    bool pendingTransportTimedOut_ = false;
    ObdTransportResult readyTransportResult_ = {};

    char bleBuf_[BLE_BUF_LEN] = {};
    size_t bleBufLen_ = 0;
    bool bleDataReady_ = false;
    bool bleDisconnected_ = false;
    int bleDisconnectReason_ = 0;
    bool bleOverflowed_ = false;
    static constexpr size_t BLE_EVENT_QUEUE_DEPTH = 8;
    std::array<BleEvent, BLE_EVENT_QUEUE_DEPTH> bleEventQueue_ = {};
    size_t bleEventQueueHead_ = 0;
    size_t bleEventQueueCount_ = 0;
    portMUX_TYPE bleEventQueueMux_ = portMUX_INITIALIZER_UNLOCKED;

#ifdef UNIT_TEST
    bool testStartScanResult_ = true;
    bool testConnectResult_ = true;
    bool testBleConnected_ = false;
    bool testDiscoverResult_ = true;
    bool testSubscribeResult_ = true;
    bool testWriteResult_ = true;
    bool testBeginSecurityResult_ = true;
    bool testSecurityReady_ = true;
    bool testSecurityEncrypted_ = true;
    bool testSecurityBonded_ = true;
    bool testSecurityAuthenticated_ = true;
    int8_t testRssi_ = 0;
    int testLastBleError_ = 0;
    int testLastSecurityError_ = 0;
    uint32_t testStartScanCalls_ = 0;
    uint32_t testConnectCalls_ = 0;
    uint32_t testDiscoverCalls_ = 0;
    uint32_t testDisconnectCalls_ = 0;
    uint32_t testWriteCalls_ = 0;
    uint32_t testBeginSecurityCalls_ = 0;
    uint32_t testDeleteBondCalls_ = 0;
    uint32_t testRefreshBondBackupCalls_ = 0;
    char testLastCommand_[CMD_BUF_LEN] = {};
    bool testLastWriteWithResponse_ = true;
#endif
};
