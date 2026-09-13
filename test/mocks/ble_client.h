#pragma once
#ifndef BLE_CLIENT_H
#define BLE_CLIENT_H

#include <stdint.h>
#include <cstring>

enum class SendResult { SENT, NOT_YET, FAILED };

/**
 * Mock V1BLEClient - tracks method calls for verification
 */
class V1BLEClient {
public:
    enum class UserBytesVerificationStatus : uint8_t { INACTIVE = 0, PENDING, MATCH, MISMATCH };
    // Call tracking
    int setMuteCalls = 0;
    bool lastMuteValue = false;
    int setVolumeCalls = 0;
    uint8_t lastVolume = 0;
    uint8_t lastMuteVolume = 0;
    int writeUserBytesCalls = 0;
    uint8_t lastUserBytes[6] = {0};
    int startUserBytesVerificationCalls = 0;
    uint8_t lastVerifiedUserBytes[6] = {0};
    int requestUserBytesCalls = 0;
    int requestCurrentVolumeCalls = 0;
    int setDisplayOnCalls = 0;
    bool lastDisplayOnValue = true;
    int setModeCalls = 0;
    uint8_t lastModeValue = 0;
    int requestAlertDataCalls = 0;
    bool alertDataRequestSent = true;
    bool requestAlertDataResult = true;
    int processProxyQueueCalls = 0;
    int onUserBytesReceivedCalls = 0;
    int onAllVolumeReceivedCalls = 0;
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
    void (*requestUserBytesSendHook)() = nullptr;
    void (*requestCurrentVolumeSendHook)() = nullptr;
    void (*setDisplayOnSendHook)() = nullptr;
    void (*setModeSendHook)() = nullptr;
    uint8_t sessionUserBytes[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t sessionUserBytesRevisionValue = 0;
    uint32_t sessionUserBytesIngressSequenceValue = 0;
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
        writeUserBytesCalls = 0;
        std::memset(lastUserBytes, 0, sizeof(lastUserBytes));
        startUserBytesVerificationCalls = 0;
        std::memset(lastVerifiedUserBytes, 0, sizeof(lastVerifiedUserBytes));
        requestUserBytesCalls = 0;
        requestCurrentVolumeCalls = 0;
        setDisplayOnCalls = 0;
        lastDisplayOnValue = true;
        setModeCalls = 0;
        lastModeValue = 0;
        requestAlertDataCalls = 0;
        alertDataRequestSent = true;
        requestAlertDataResult = true;
        processProxyQueueCalls = 0;
        onUserBytesReceivedCalls = 0;
        onAllVolumeReceivedCalls = 0;
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
        requestUserBytesSendHook = nullptr;
        requestCurrentVolumeSendHook = nullptr;
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
        setVolumeCalls++;
        lastVolume = vol;
        lastMuteVolume = muteVol;
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
        setDisplayOnCalls++;
        lastDisplayOnValue = displayOn;
        if (setDisplayOnSendHook) setDisplayOnSendHook();
        if (setDisplayOnFailuresRemaining > 0) {
            setDisplayOnFailuresRemaining--;
            return false;
        }
        return setDisplayOnResult;
    }

    bool setMode(uint8_t mode) {
        setModeCalls++;
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
        hasSessionUserBytesFlag = true;
        if (bytes) std::memcpy(sessionUserBytes, bytes, sizeof(sessionUserBytes));
        ++sessionUserBytesRevisionValue;
        sessionUserBytesIngressSequenceValue = ingressSequence;
    }
    void onAllVolumeReceived() {
        onAllVolumeReceivedCalls++;
        hasSessionAllVolumeFlag = true;
    }

    void resetSessionSettingsCapture() {
        hasSessionUserBytesFlag = false;
        hasSessionAllVolumeFlag = false;
        sessionUserBytesRevisionValue = 0;
        sessionUserBytesIngressSequenceValue = 0;
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
