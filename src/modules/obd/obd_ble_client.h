#pragma once

/// @file obd_ble_client.h
/// OBD-owned BLE client and scan callback, fully independent of ble_client.cpp.
/// Owns its own NimBLEClient. Created once, never deleted (heap-safety rule).

#include <NimBLEDevice.h>
#include <atomic>
#include <cstdint>

#include "obd_transport_link_fence.h"

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

    /// Request a graceful disconnect. Returns false only when NimBLE rejects
    /// the terminate/cancel command; callers must still wait for the active
    /// link generation to be confirmed down.
    bool disconnect();

    uint32_t activeLinkGeneration() const { return activeLinkGeneration_.load(std::memory_order_acquire); }
    bool linkDownConfirmed(uint32_t generation) const {
        if (confirmedDownGeneration_.load(std::memory_order_acquire) != generation) {
            return false;
        }
        return !pClient_ || (!pClient_->isConnected() && pClient_->getConnHandle() == static_cast<uint16_t>(-1));
    }

    /// True if GATT client is connected.
    bool isConnected() const;
    bool beginSecurity();
    bool isSecurityReady() const;
    bool isEncrypted() const;
    bool isBonded() const;
    bool isAuthenticated() const;
    int getLastBleError() const { return lastBleError_.load(std::memory_order_acquire); }
    int getLastSecurityError() const { return lastSecurityError_.load(std::memory_order_acquire); }
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

    /// Consume callback-published link-down state and clear GATT handles.
    /// This is transport-task-only: callbacks and the main loop must never
    /// mutate characteristic ownership directly.
    void serviceDeferredLinkState();

#ifdef V1_LINKED_TEST_OBD_BLE_CLIENT
    bool hasCharacteristicHandlesForTest() const { return pTxChar_ != nullptr && pRxChar_ != nullptr; }
#endif

  private:
    friend class ObdClientCallback;

    void syncSecurityStateFromConnInfo();
    void handleConnected();
    void handleDisconnected(int reason);
    void handleAuthenticationComplete(const NimBLEConnInfo& connInfo);
    void handleIdentityResolved(const NimBLEConnInfo& connInfo);
    void clearLinkState(bool clearErrors);
    void clearCharacteristicHandles();

    NimBLEClient* pClient_ = nullptr;
    NimBLERemoteCharacteristic* pTxChar_ = nullptr; // notify (device → host)
    NimBLERemoteCharacteristic* pRxChar_ = nullptr; // write  (host → device)

    ObdScanCallback scanCallback_;
    ObdClientCallback clientCallback_;

    ObdTransportLinkFence linkDownFence_;
    std::atomic<uint32_t> activeLinkGeneration_{0};
    std::atomic<uint32_t> confirmedDownGeneration_{0};
    int8_t cachedRssi_ = 0;
    uint32_t lastRssiQueryMs_ = 0;
    std::atomic<bool> connectPending_{false};
    std::atomic<bool> securityPending_{false};
    std::atomic<bool> securityReady_{false};
    std::atomic<bool> encrypted_{false};
    std::atomic<bool> bonded_{false};
    std::atomic<bool> authenticated_{false};
    std::atomic<int> lastBleError_{0};
    std::atomic<int> lastSecurityError_{0};
    static constexpr uint32_t RSSI_QUERY_INTERVAL_MS = 2000;
};
