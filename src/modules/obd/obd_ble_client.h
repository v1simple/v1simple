#pragma once

/// @file obd_ble_client.h
/// OBD-owned BLE client and scan callback, fully independent of ble_client.cpp.
/// Owns its own NimBLEClient. Created once, never deleted (heap-safety rule).


#include <NimBLEDevice.h>
#include <cstdint>

class ObdRuntimeModule;

/// Scan callback that filters for OBDLink devices by name prefix + RSSI gate.
class ObdScanCallback : public NimBLEScanCallbacks {
public:
    void configure(ObdRuntimeModule* parent, int8_t minRssi);
    void onResult(const NimBLEAdvertisedDevice* device) override;
    void onScanEnd(const NimBLEScanResults& results, int reason) override;

private:
    ObdRuntimeModule* parent_ = nullptr;
    int8_t minRssi_ = -80;
};

/// OBD client disconnect callback — signals runtime module on disconnect.
class ObdClientCallback : public NimBLEClientCallbacks {
public:
    void configure(class ObdBleClient* owner, ObdRuntimeModule* parent);
    void onConnect(NimBLEClient* client) override;
    void onConnectFail(NimBLEClient* client, int reason) override;
    void onDisconnect(NimBLEClient* client, int reason) override;
    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;
    void onIdentity(NimBLEConnInfo& connInfo) override;

private:
    class ObdBleClient* owner_ = nullptr;
    ObdRuntimeModule* parent_ = nullptr;
};

/// Low-level BLE operations for OBD adapter communication.
class ObdBleClient {
public:
    /// Create the NimBLEClient (once). Must be called after NimBLEDevice::init().
    void init(ObdRuntimeModule* parent);

    /// Start a BLE scan filtered for OBDLink devices. Returns false if scanner busy.
    /// @param parent Callback target for device discovery events.
    bool startScan(ObdRuntimeModule* parent, int8_t minRssi);

    /// Stop any in-progress OBD scan.
    void stopScan();

    /// Connect to OBD adapter at the given address. Non-blocking start, but
    /// the actual connection completes asynchronously.
    bool connect(const char* address, uint8_t addrType, uint32_t timeoutMs, bool preferCachedAttributes);

    /// Disconnect gracefully.
    void disconnect();

    /// True if GATT client is connected.
    bool isConnected() const;
    bool beginSecurity();
    bool isSecurityReady() const;
    bool isEncrypted() const;
    bool isBonded() const;
    bool isAuthenticated() const;
    int getLastBleError() const { return lastBleError_; }
    int getLastSecurityError() const { return lastSecurityError_; }
    bool deleteBond(const char* address, uint8_t addrType);

    /// Discover SPP-over-GATT service and TX/RX characteristics.
    /// Returns true if both characteristics found.
    bool discoverServices();

    /// Write an AT/OBD command string to the RX characteristic (host→device).
    bool writeCommand(const char* cmd, bool withResponse);

    /// Subscribe to TX notify (device→host). Callback receives response data.
    bool subscribeNotify(void (*callback)(const uint8_t* data, size_t len));

    /// Query cached RSSI. Updates at most every 2 seconds.
    int8_t getRssi(uint32_t nowMs);

private:
    friend class ObdClientCallback;

    void syncSecurityStateFromConnInfo();
    void handleConnected();
    void handleDisconnected(int reason);
    void handleAuthenticationComplete(const NimBLEConnInfo& connInfo);
    void handleIdentityResolved(const NimBLEConnInfo& connInfo);
    void clearLinkState(bool clearErrors);

    NimBLEClient* pClient_ = nullptr;
    NimBLERemoteCharacteristic* pTxChar_ = nullptr;  // notify (device → host)
    NimBLERemoteCharacteristic* pRxChar_ = nullptr;  // write  (host → device)

    ObdScanCallback scanCallback_;
    ObdClientCallback clientCallback_;

    int8_t cachedRssi_ = 0;
    uint32_t lastRssiQueryMs_ = 0;
    bool connectPending_ = false;
    bool securityPending_ = false;
    bool securityReady_ = false;
    bool encrypted_ = false;
    bool bonded_ = false;
    bool authenticated_ = false;
    int lastBleError_ = 0;
    int lastSecurityError_ = 0;
    static constexpr uint32_t RSSI_QUERY_INTERVAL_MS = 2000;
};
