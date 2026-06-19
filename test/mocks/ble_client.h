#pragma once
#ifndef BLE_CLIENT_H
#define BLE_CLIENT_H

#include <stdint.h>
#include <cstring>

/**
 * Mock V1BLEClient - tracks method calls for verification
 */
class V1BLEClient {
public:
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
    int setDisplayOnCalls = 0;
    bool lastDisplayOnValue = true;
    int setModeCalls = 0;
    uint8_t lastModeValue = 0;
    int requestAlertDataCalls = 0;
    int processProxyQueueCalls = 0;
    int onUserBytesReceivedCalls = 0;
    bool bootReadyFlag = true;  // Default true to preserve existing test behavior
    bool connectBurstSettling = false;
    int connectionRssi = -70;
    int proxyRssi = -80;
    bool writeUserBytesResult = true;
    int writeUserBytesFailuresRemaining = 0;
    bool setDisplayOnResult = true;
    int setDisplayOnFailuresRemaining = 0;
    bool setModeResult = true;
    int setModeFailuresRemaining = 0;
    bool setVolumeResult = true;
    int setVolumeFailuresRemaining = 0;
    int setWifiPriorityCalls = 0;
    bool lastWifiPriorityValue = false;
    
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
        setDisplayOnCalls = 0;
        lastDisplayOnValue = true;
        setModeCalls = 0;
        lastModeValue = 0;
        requestAlertDataCalls = 0;
        processProxyQueueCalls = 0;
        onUserBytesReceivedCalls = 0;
        bootReadyFlag = true;
        connectBurstSettling = false;
        connectionRssi = -70;
        proxyRssi = -80;
        writeUserBytesResult = true;
        writeUserBytesFailuresRemaining = 0;
        setDisplayOnResult = true;
        setDisplayOnFailuresRemaining = 0;
        setModeResult = true;
        setModeFailuresRemaining = 0;
        setVolumeResult = true;
        setVolumeFailuresRemaining = 0;
        wifiPriorityMode = false;
        setWifiPriorityCalls = 0;
        lastWifiPriorityValue = false;
    }
    
    // Connection state
    bool isProxyClientConnected() const { return proxyConnected; }
    void setProxyConnected(bool v) { proxyConnected = v; }
    bool isConnected() const { return connected; }
    void setConnected(bool v) { connected = v; }
    bool isWifiPriority() const { return wifiPriorityMode; }
    void setWifiPriority(bool enabled) {
        wifiPriorityMode = enabled;
        lastWifiPriorityValue = enabled;
        setWifiPriorityCalls++;
    }
    void setWifiPriorityForTest(bool enabled) { wifiPriorityMode = enabled; }
    bool isConnectBurstSettling() const { return connectBurstSettling; }
    void setConnectBurstSettling(bool v) { connectBurstSettling = v; }
    void setBootReady(bool ready) { bootReadyFlag = ready; }
    bool isBootReady() const { return bootReadyFlag; }
    int getConnectionRssi() const { return connectionRssi; }
    int getProxyClientRssi() const { return proxyRssi; }
    void setConnectionRssi(int rssi) { connectionRssi = rssi; }
    void setProxyRssi(int rssi) { proxyRssi = rssi; }
    
    // BLE commands (tracked)
    bool setMute(bool mute) { 
        setMuteCalls++; 
        lastMuteValue = mute;
        return true; 
    }
    
    bool setVolume(uint8_t vol, uint8_t muteVol) { 
        setVolumeCalls++;
        lastVolume = vol;
        lastMuteVolume = muteVol;
        if (setVolumeFailuresRemaining > 0) {
            setVolumeFailuresRemaining--;
            return false;
        }
        return setVolumeResult; 
    }

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

    void startUserBytesVerification(const uint8_t* bytes) {
        startUserBytesVerificationCalls++;
        if (bytes) {
            std::memcpy(lastVerifiedUserBytes, bytes, sizeof(lastVerifiedUserBytes));
        }
    }

    void requestUserBytes() {
        requestUserBytesCalls++;
    }

    bool setDisplayOn(bool displayOn) {
        setDisplayOnCalls++;
        lastDisplayOnValue = displayOn;
        if (setDisplayOnFailuresRemaining > 0) {
            setDisplayOnFailuresRemaining--;
            return false;
        }
        return setDisplayOnResult;
    }

    bool setMode(uint8_t mode) {
        setModeCalls++;
        lastModeValue = mode;
        if (setModeFailuresRemaining > 0) {
            setModeFailuresRemaining--;
            return false;
        }
        return setModeResult;
    }
    
    void requestAlertData() {
        requestAlertDataCalls++;
    }

    void processProxyQueue() {
        processProxyQueueCalls++;
    }

    void onUserBytesReceived(const uint8_t* /*bytes*/) {
        onUserBytesReceivedCalls++;
    }
    
private:
    bool proxyConnected = false;
    bool connected = true;
    bool wifiPriorityMode = false;
};

#endif  // BLE_CLIENT_H
