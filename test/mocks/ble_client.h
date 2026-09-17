#pragma once
#ifndef BLE_CLIENT_H
#define BLE_CLIENT_H

#include <stdint.h>
#include <cstring>
#include <vector>

enum class SendResult { SENT, NOT_YET, FAILED };

/**
 * Mock V1BLEClient - tracks method calls for verification
 */
class V1BLEClient {
public:
    struct SweepWrite {
        uint8_t index;
        uint16_t lower;
        uint16_t upper;
        bool commit;
    };
    enum class UserBytesVerificationStatus : uint8_t { INACTIVE = 0, PENDING, MATCH, MISMATCH };
    // Call tracking
    int setMuteCalls = 0;
    bool lastMuteValue = false;
    int setVolumeCalls = 0;
    uint8_t lastVolume = 0;
    uint8_t lastMuteVolume = 0;
    uint8_t lastVolumeAux = 0;
    int writeUserBytesCalls = 0;
    uint8_t lastUserBytes[6] = {0};
    int startUserBytesVerificationCalls = 0;
    uint8_t lastVerifiedUserBytes[6] = {0};
    int requestUserBytesCalls = 0;
    int requestCurrentVolumeCalls = 0;
    int requestAllVolumeCalls = 0;
    int setDisplayOnCalls = 0;
    bool lastDisplayOnValue = true;
    bool lastKeepBluetoothIndicatorOn = false;
    int setModeCalls = 0;
    uint8_t lastModeValue = 0;
    int requestAlertDataCalls = 0;
    bool alertDataRequestSent = true;
    bool requestAlertDataResult = true;
    int processProxyQueueCalls = 0;
    int onUserBytesReceivedCalls = 0;
    int onAllVolumeReceivedCalls = 0;
    int onV1DisplayFlowControlCalls = 0;
    bool lastTimeSliceHoldoff = true;
    bool hasSessionUserBytesFlag = false;
    bool hasSessionAllVolumeFlag = false;
    bool bootReadyFlag = true;  // Default true to preserve existing test behavior
    bool connectBurstSettling = false;
    uint32_t sessionGenerationValue = 1;
    uint32_t firmwareVersion = 0;
    int connectionRssi = -70;
    int proxyRssi = -80;
    bool writeUserBytesResult = true;
    int writeUserBytesFailuresRemaining = 0;
    bool setDisplayOnResult = true;
    int setDisplayOnFailuresRemaining = 0;
    bool setModeResult = true;
    int setModeFailuresRemaining = 0;
    bool setVolumeSuccess = true;
    int setVolumeFailuresRemaining = 0;
    SendResult nextMuteSendResult = SendResult::SENT;
    SendResult nextVolumeSendResult = SendResult::SENT;
    bool requestUserBytesResult = true;
    bool requestCurrentVolumeResult = true;
    bool requestAllVolumeResult = true;
    int requestAllSweepDefinitionsCalls = 0;
    int writeSweepDefinitionCalls = 0;
    bool requestAllSweepDefinitionsResult = true;
    SendResult writeSweepDefinitionResult = SendResult::SENT;
    uint8_t lastSweepIndex = 0;
    uint16_t lastSweepLower = 0;
    uint16_t lastSweepUpper = 0;
    bool lastSweepCommit = false;
    std::vector<SweepWrite> sweepWriteHistory;
    std::vector<const char*> commandHistory;
    uint32_t sweepSectionsBoundary = 0;
    uint32_t sweepMaxBoundary = 0;
    uint32_t sweepDefinitionsBoundary = 0;
    bool sweepSectionsResetPending = false;
    bool sweepMaxResetPending = false;
    bool sweepDefinitionsResetPending = false;
    bool sessionSweepMaxCaptured = false;
    bool sessionSweepSectionsCaptured = false;
    bool sessionSweepDefinitionsCaptured = false;
    void (*requestUserBytesSendHook)() = nullptr;
    void (*requestCurrentVolumeSendHook)() = nullptr;
    void (*requestAllVolumeSendHook)() = nullptr;
    void (*requestAllSweepDefinitionsSendHook)() = nullptr;
    void (*setDisplayOnSendHook)() = nullptr;
    void (*setModeSendHook)() = nullptr;
    uint8_t sessionUserBytes[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t sessionUserBytesRevisionValue = 0;
    uint32_t sessionUserBytesIngressSequenceValue = 0;
    uint32_t userBytesBoundary = 0;
    uint32_t allVolumeBoundary = 0;
    bool userBytesCaptureArmed = false;
    bool allVolumeCaptureArmed = false;
    uint32_t v1NotificationIngressSequenceValue = 0;
    uint32_t verifiedSettingsApplyGenerationValue = 0;
    UserBytesVerificationStatus verificationStatus = UserBytesVerificationStatus::INACTIVE;
    int cancelUserBytesVerificationCalls = 0;
    int publishVerifiedSettingsApplyEdgeCalls = 0;
    bool verifiedSettingsApplyEdgePending = false;
    mutable bool changeSessionOnNextUserBytesCopy = false;
    bool changeSessionDuringPublish = false;
    
    void reset() {
        proxyConnected = false;
        connected = true;
        setMuteCalls = 0;
        lastMuteValue = false;
        setVolumeCalls = 0;
        lastVolume = 0;
        lastMuteVolume = 0;
        lastVolumeAux = 0;
        writeUserBytesCalls = 0;
        std::memset(lastUserBytes, 0, sizeof(lastUserBytes));
        startUserBytesVerificationCalls = 0;
        std::memset(lastVerifiedUserBytes, 0, sizeof(lastVerifiedUserBytes));
        requestUserBytesCalls = 0;
        requestCurrentVolumeCalls = 0;
        requestAllVolumeCalls = 0;
        setDisplayOnCalls = 0;
        lastDisplayOnValue = true;
        lastKeepBluetoothIndicatorOn = false;
        setModeCalls = 0;
        lastModeValue = 0;
        requestAlertDataCalls = 0;
        alertDataRequestSent = true;
        requestAlertDataResult = true;
        processProxyQueueCalls = 0;
        onUserBytesReceivedCalls = 0;
        onAllVolumeReceivedCalls = 0;
        onV1DisplayFlowControlCalls = 0;
        lastTimeSliceHoldoff = true;
        hasSessionUserBytesFlag = false;
        hasSessionAllVolumeFlag = false;
        bootReadyFlag = true;
        connectBurstSettling = false;
        sessionGenerationValue = 1;
        firmwareVersion = 0;
        connectionRssi = -70;
        proxyRssi = -80;
        writeUserBytesResult = true;
        writeUserBytesFailuresRemaining = 0;
        setDisplayOnResult = true;
        setDisplayOnFailuresRemaining = 0;
        setModeResult = true;
        setModeFailuresRemaining = 0;
        setVolumeSuccess = true;
        setVolumeFailuresRemaining = 0;
        nextMuteSendResult = SendResult::SENT;
        nextVolumeSendResult = SendResult::SENT;
        requestUserBytesResult = true;
        requestCurrentVolumeResult = true;
        requestAllVolumeResult = true;
        requestAllSweepDefinitionsCalls = 0;
        writeSweepDefinitionCalls = 0;
        requestAllSweepDefinitionsResult = true;
        writeSweepDefinitionResult = SendResult::SENT;
        sweepWriteHistory.clear();
        commandHistory.clear();
        sessionSweepMaxCaptured = false;
        sessionSweepSectionsCaptured = false;
        sessionSweepDefinitionsCaptured = false;
        requestUserBytesSendHook = nullptr;
        requestCurrentVolumeSendHook = nullptr;
        requestAllVolumeSendHook = nullptr;
        requestAllSweepDefinitionsSendHook = nullptr;
        setDisplayOnSendHook = nullptr;
        setModeSendHook = nullptr;
        std::memset(sessionUserBytes, 0xFF, sizeof(sessionUserBytes));
        sessionUserBytesRevisionValue = 0;
        sessionUserBytesIngressSequenceValue = 0;
        v1NotificationIngressSequenceValue = 0;
        verifiedSettingsApplyGenerationValue = 0;
        verificationStatus = UserBytesVerificationStatus::INACTIVE;
        cancelUserBytesVerificationCalls = 0;
        publishVerifiedSettingsApplyEdgeCalls = 0;
        verifiedSettingsApplyEdgePending = false;
        changeSessionOnNextUserBytesCopy = false;
        changeSessionDuringPublish = false;
    }
    
    // Connection state
    bool isProxyClientConnected() const { return proxyConnected; }
    void setProxyConnected(bool v) { proxyConnected = v; }
    bool isConnected() const { return connected; }
    void onV1DisplayFlowControl(bool holdoff) {
        onV1DisplayFlowControlCalls++;
        lastTimeSliceHoldoff = holdoff;
    }
    void setConnected(bool v) { connected = v; }
    uint32_t sessionGeneration() const { return sessionGenerationValue; }
    void setSessionGeneration(uint32_t generation) { sessionGenerationValue = generation; }
    bool isConnectBurstSettling() const { return connectBurstSettling; }
    void setConnectBurstSettling(bool v) { connectBurstSettling = v; }
    void setBootReady(bool ready) { bootReadyFlag = ready; }
    bool isBootReady() const { return bootReadyFlag; }
    int getConnectionRssi() const { return connectionRssi; }
    int getProxyClientRssi() const { return proxyRssi; }
    void setConnectionRssi(int rssi) { connectionRssi = rssi; }
    void setProxyRssi(int rssi) { proxyRssi = rssi; }
    
    // BLE commands (tracked)
    SendResult setMuteResult(bool mute) {
        setMuteCalls++; 
        lastMuteValue = mute;
        const SendResult result = nextMuteSendResult;
        nextMuteSendResult = SendResult::SENT;
        return result;
    }
    bool setMute(bool mute) { return setMuteResult(mute) == SendResult::SENT; }
    
    SendResult setVolumeResult(uint8_t vol, uint8_t muteVol) {
        return setVolumeResult(vol, muteVol, 0);
    }
    SendResult setVolumeResult(uint8_t vol, uint8_t muteVol, uint8_t aux) {
        setVolumeCalls++;
        commandHistory.push_back("volume-write");
        lastVolume = vol;
        lastMuteVolume = muteVol;
        lastVolumeAux = aux;
        if (nextVolumeSendResult != SendResult::SENT) {
            const SendResult result = nextVolumeSendResult;
            nextVolumeSendResult = SendResult::SENT;
            return result;
        }
        if (setVolumeFailuresRemaining > 0) {
            setVolumeFailuresRemaining--;
            return SendResult::FAILED;
        }
        return setVolumeSuccess ? SendResult::SENT : SendResult::FAILED;
    }
    bool setVolume(uint8_t vol, uint8_t muteVol) { return setVolumeResult(vol, muteVol) == SendResult::SENT; }

    bool writeUserBytes(const uint8_t* bytes) {
        writeUserBytesCalls++;
        commandHistory.push_back("user-write");
        if (bytes) {
            std::memcpy(lastUserBytes, bytes, sizeof(lastUserBytes));
        }
        if (writeUserBytesFailuresRemaining > 0) {
            writeUserBytesFailuresRemaining--;
            return false;
        }
        return writeUserBytesResult;
    }

    bool writeUserBytesExact(const uint8_t* bytes) { return writeUserBytes(bytes); }

    void startUserBytesVerification(const uint8_t* bytes) {
        startUserBytesVerificationCalls++;
        if (bytes) {
            std::memcpy(lastVerifiedUserBytes, bytes, sizeof(lastVerifiedUserBytes));
        }
        verificationStatus = UserBytesVerificationStatus::PENDING;
    }

    void onV1FirmwareVersionReceived(uint32_t version) { firmwareVersion = version; }
    uint32_t v1FirmwareVersion() const { return firmwareVersion; }
    bool hasV1FirmwareVersion() const { return firmwareVersion != 0; }

    bool requestUserBytes() {
        requestUserBytesCalls++;
        if (requestUserBytesSendHook) requestUserBytesSendHook();
        return requestUserBytesResult;
    }

    bool requestCurrentVolume() {
        requestCurrentVolumeCalls++;
        if (requestCurrentVolumeSendHook) requestCurrentVolumeSendHook();
        return requestCurrentVolumeResult;
    }
    bool requestAllVolume() {
        ++requestAllVolumeCalls;
        commandHistory.push_back("volume-read");
        if (requestAllVolumeSendHook) requestAllVolumeSendHook();
        return requestAllVolumeResult;
    }
    bool requestAllSweepDefinitions() {
        ++requestAllSweepDefinitionsCalls;
        commandHistory.push_back("sweep-read");
        if (requestAllSweepDefinitionsSendHook) requestAllSweepDefinitionsSendHook();
        return requestAllSweepDefinitionsResult;
    }
    SendResult writeSweepDefinition(uint8_t index, uint16_t lower, uint16_t upper, bool commit) {
        ++writeSweepDefinitionCalls;
        lastSweepIndex = index;
        lastSweepLower = lower;
        lastSweepUpper = upper;
        lastSweepCommit = commit;
        sweepWriteHistory.push_back({index, lower, upper, commit});
        commandHistory.push_back(commit ? "sweep-commit" : "sweep-write");
        return writeSweepDefinitionResult;
    }

    UserBytesVerificationStatus userBytesVerificationStatus() const { return verificationStatus; }
    void setUserBytesVerificationStatus(UserBytesVerificationStatus status) { verificationStatus = status; }
    void cancelUserBytesVerification() {
        cancelUserBytesVerificationCalls++;
        verificationStatus = UserBytesVerificationStatus::INACTIVE;
    }
    void publishVerifiedSettingsApplyEdge(uint32_t verifiedSessionGeneration) {
        ++publishVerifiedSettingsApplyEdgeCalls;
        verifiedSettingsApplyGenerationValue = verifiedSessionGeneration;
        if (changeSessionDuringPublish) {
            ++sessionGenerationValue;
            changeSessionDuringPublish = false;
        }
        verifiedSettingsApplyEdgePending = true;
    }
    bool consumeVerifyPushMatchEdge() {
        const bool pending = verifiedSettingsApplyEdgePending;
        verifiedSettingsApplyEdgePending = false;
        return pending && verifiedSettingsApplyGenerationValue == sessionGenerationValue;
    }
    uint32_t noteV1NotificationIngress() {
        ++v1NotificationIngressSequenceValue;
        if (v1NotificationIngressSequenceValue == 0) ++v1NotificationIngressSequenceValue;
        return v1NotificationIngressSequenceValue;
    }
    uint32_t latestV1NotificationIngressSequence() const { return v1NotificationIngressSequenceValue; }

    bool setDisplayOn(bool displayOn) {
        return setDisplayOn(displayOn, false);
    }
    bool setDisplayOn(bool displayOn, bool keepBluetoothIndicatorOn) {
        setDisplayOnCalls++;
        commandHistory.push_back("display-write");
        lastDisplayOnValue = displayOn;
        lastKeepBluetoothIndicatorOn = keepBluetoothIndicatorOn;
        if (setDisplayOnSendHook) setDisplayOnSendHook();
        if (setDisplayOnFailuresRemaining > 0) {
            setDisplayOnFailuresRemaining--;
            return false;
        }
        return setDisplayOnResult;
    }

    bool setMode(uint8_t mode) {
        setModeCalls++;
        commandHistory.push_back("mode-write");
        lastModeValue = mode;
        if (setModeSendHook) setModeSendHook();
        if (setModeFailuresRemaining > 0) {
            setModeFailuresRemaining--;
            return false;
        }
        return setModeResult;
    }
    
    void requestAlertData() {
        requestAlertDataCalls++;
        if (requestAlertDataResult) {
            alertDataRequestSent = true;
        }
    }
    bool needsAlertDataStartRecovery() const { return !alertDataRequestSent && !connectBurstSettling; }

    void processProxyQueue() {
        processProxyQueueCalls++;
    }

    void onUserBytesReceived(const uint8_t* bytes, uint32_t ingressSequence = 0) {
        onUserBytesReceivedCalls++;
        if (userBytesCaptureArmed && ingressSequence != 0 &&
            static_cast<int32_t>(ingressSequence - userBytesBoundary) > 0) {
            hasSessionUserBytesFlag = true;
            if (bytes) std::memcpy(sessionUserBytes, bytes, sizeof(sessionUserBytes));
            ++sessionUserBytesRevisionValue;
            sessionUserBytesIngressSequenceValue = ingressSequence;
        }
    }
    void onAllVolumeReceived(uint32_t ingressSequence) {
        onAllVolumeReceivedCalls++;
        if (allVolumeCaptureArmed && ingressSequence != 0 &&
            static_cast<int32_t>(ingressSequence - allVolumeBoundary) > 0) {
            hasSessionAllVolumeFlag = true;
        }
    }
    void beginSessionUserBytesCapture(uint32_t boundary) {
        userBytesBoundary = boundary;
        userBytesCaptureArmed = true;
        hasSessionUserBytesFlag = false;
        sessionUserBytesIngressSequenceValue = 0;
    }
    void beginSessionAllVolumeCapture(uint32_t boundary) {
        allVolumeBoundary = boundary;
        allVolumeCaptureArmed = true;
        hasSessionAllVolumeFlag = false;
    }
    void beginSessionSweepSectionsCapture(uint32_t boundary) {
        sweepSectionsBoundary = boundary;
        sweepSectionsResetPending = true;
        sessionSweepSectionsCaptured = false;
    }
    void beginSessionSweepMaxCapture(uint32_t boundary) {
        sweepMaxBoundary = boundary;
        sweepMaxResetPending = true;
        sessionSweepMaxCaptured = false;
    }
    void beginSessionSweepDefinitionsCapture(uint32_t boundary) {
        sweepDefinitionsBoundary = boundary;
        sweepDefinitionsResetPending = true;
        sessionSweepDefinitionsCaptured = false;
    }
    bool sessionSweepResponseEligible(uint8_t packetId, uint32_t ingress) const {
        const uint32_t boundary = packetId == 0x23 ? sweepSectionsBoundary :
                                  packetId == 0x20 ? sweepMaxBoundary :
                                  packetId == 0x17 ? sweepDefinitionsBoundary : 0;
        return boundary != 0 && static_cast<int32_t>(ingress - boundary) > 0;
    }
    bool consumeSessionSweepParserReset(uint8_t packetId) {
        bool* pending = packetId == 0x23 ? &sweepSectionsResetPending :
                        packetId == 0x20 ? &sweepMaxResetPending : &sweepDefinitionsResetPending;
        const bool value = *pending;
        *pending = false;
        return value;
    }
    void onSweepSectionsReceived(bool complete) { sessionSweepSectionsCaptured = complete; }
    void onSweepMaxReceived(bool complete = true) { sessionSweepMaxCaptured = complete; }
    void onSweepDefinitionsReceived(bool complete) { sessionSweepDefinitionsCaptured = complete; }
    bool hasSessionSweepMaxCapture() const { return sessionSweepMaxCaptured; }
    bool hasSessionSweepSectionsCapture() const { return sessionSweepSectionsCaptured; }
    bool hasSessionSweepDefinitionsCapture() const { return sessionSweepDefinitionsCaptured; }
    uint32_t sessionSweepDefinitionsIngressBoundary() const { return sweepDefinitionsBoundary; }

    void resetSessionSettingsCapture() {
        hasSessionUserBytesFlag = false;
        hasSessionAllVolumeFlag = false;
        sessionUserBytesRevisionValue = 0;
        sessionUserBytesIngressSequenceValue = 0;
        userBytesBoundary = 0;
        allVolumeBoundary = 0;
        userBytesCaptureArmed = false;
        allVolumeCaptureArmed = false;
        sessionSweepSectionsCaptured = false;
        sessionSweepMaxCaptured = false;
        sessionSweepDefinitionsCaptured = false;
    }
    bool hasSessionUserBytes() const { return hasSessionUserBytesFlag; }
    bool hasSessionAllVolume() const { return hasSessionAllVolumeFlag; }
    bool copySessionUserBytes(uint8_t out[6]) const {
        if (!out || !hasSessionUserBytesFlag) return false;
        std::memcpy(out, sessionUserBytes, sizeof(sessionUserBytes));
        if (changeSessionOnNextUserBytesCopy) {
            changeSessionOnNextUserBytesCopy = false;
            ++const_cast<V1BLEClient*>(this)->sessionGenerationValue;
        }
        return true;
    }
    uint32_t sessionUserBytesRevision() const { return sessionUserBytesRevisionValue; }
    uint32_t sessionUserBytesIngressSequence() const { return sessionUserBytesIngressSequenceValue; }
    bool settingsCaptureTimedOut() const { return false; }
    
private:
    bool proxyConnected = false;
    bool connected = true;
};

#endif  // BLE_CLIENT_H
