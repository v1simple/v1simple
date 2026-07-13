#pragma once

#include <Arduino.h>

// Forward declarations
class V1BLEClient;
class PacketParser;
class V1Display;
class PowerModule;
class BleQueueModule;
class SystemEventBus;

/**
 * ConnectionStateModule - Tracks V1 BLE connection state transitions
 *
 * Responsibilities:
 * - Detect connect/disconnect transitions
 * - Reset parser state on disconnect
 * - Re-request alert data when traffic stops
 * - Notify power module of connection changes
 * - Refresh display indicators when disconnected
 */
class ConnectionStateModule {
public:
    void begin(V1BLEClient* bleClient,
               PacketParser* parser,
               V1Display* display,
               PowerModule* powerModule,
               BleQueueModule* bleQueueModule,
               SystemEventBus* eventBus = nullptr);

    // Call once per loop iteration; returns true if connected
    bool process(unsigned long nowMs);

private:
    V1BLEClient* ble_ = nullptr;
    PacketParser* parser_ = nullptr;
    V1Display* display_ = nullptr;
    PowerModule* power_ = nullptr;
    BleQueueModule* bleQueue_ = nullptr;
    SystemEventBus* bus_ = nullptr;

    bool wasConnected_ = false;
    unsigned long lastDataRequestMs_ = 0;

    static constexpr unsigned long DATA_STALE_MS = 2000;      // Consider data stale after 2s
    static constexpr unsigned long DATA_REQUEST_INTERVAL_MS = 1000;  // Re-request every 1s when stale
};
