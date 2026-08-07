#pragma once
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H
// ============================================================================
// Minimal WiFiManager stub for native unit tests.
// Uses #ifndef guard (matching the real wifi_manager.h) so including this
// file first blocks the real wifi_manager.h from compiling.
// ============================================================================

class WiFiManager {
public:
    bool isWifiServiceActive() const { return wifiServiceActive_; }
    bool isConnected() const         { return staConnected_; }
    bool isSetupModeActive() const   { return apActive_; }
    bool isReconnectGaveUp() const   { return reconnectGaveUp_; }
    bool hasPendingLifecycleWork() const { return pendingLifecycleWork_; }
    bool isUiActive(unsigned long timeoutMs = 30000) const {
        lastUiTimeoutMs_ = timeoutMs;
        return uiActive_;
    }

    // Test helpers
    void setWifiServiceActive(bool v) { wifiServiceActive_ = v; }
    void setConnected(bool v)         { staConnected_ = v; }
    void setSetupModeActive(bool v)   { apActive_ = v; }
    void setReconnectGaveUp(bool v)   { reconnectGaveUp_ = v; }
    void setPendingLifecycleWork(bool v) { pendingLifecycleWork_ = v; }
    void setUiActive(bool v) { uiActive_ = v; }
    unsigned long lastUiTimeoutMs() const { return lastUiTimeoutMs_; }

private:
    bool wifiServiceActive_ = false;
    bool staConnected_       = false;
    bool apActive_           = false;
    bool reconnectGaveUp_    = false;
    bool pendingLifecycleWork_ = false;
    bool uiActive_ = false;
    mutable unsigned long lastUiTimeoutMs_ = 0;
};

extern WiFiManager wifiManager;

#endif  // WIFI_MANAGER_H
