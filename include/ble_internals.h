/**
 * Shared BLE synchronization, UUID, logging, and runtime utilities.
 */

#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

// Forward declaration
class V1BLEClient;

// ============================================================================
// SemaphoreGuard - RAII mutex wrapper for BLE operations
// ============================================================================
// The default is a non-blocking try-lock. Callers that may wait must provide a
// bounded timeout explicitly; BLE paths never use portMAX_DELAY.
class SemaphoreGuard {
  public:
    // timeout=0 performs a try-lock; positive values specify a bounded wait.
    explicit SemaphoreGuard(SemaphoreHandle_t sem, TickType_t timeout = 0) : sem_(sem), locked_(false) {
        if (sem_) {
            locked_ = xSemaphoreTake(sem_, timeout) == pdTRUE;
        }
    }
    ~SemaphoreGuard() {
        if (sem_ && locked_) {
            xSemaphoreGive(sem_);
        }
    }
    bool locked() const { return locked_; }

  private:
    SemaphoreHandle_t sem_;
    bool locked_;
};

// ============================================================================
// UUID Utilities
// ============================================================================

// Extract 16-bit short UUID from V1's 128-bit custom UUID
inline bool extractV1ShortUuidFrom128(const NimBLEUUID& uuid, uint16_t& out) {
    if (uuid.bitSize() != BLE_UUID_TYPE_128) {
        return false;
    }
    const uint8_t* raw = uuid.getValue();
    if (!raw) {
        return false;
    }

    // NimBLE stores UUID128 in little-endian form.
    // V1 base UUID suffix is "-9E05-11E2-AA59-F23C91AEC05E" => first 12 bytes below.
    static constexpr uint8_t kV1UuidBaseLePrefix[12] = {0x5E, 0xC0, 0xAE, 0x91, 0x3C, 0xF2,
                                                        0x59, 0xAA, 0xE2, 0x11, 0x05, 0x9E};
    if (memcmp(raw, kV1UuidBaseLePrefix, sizeof(kV1UuidBaseLePrefix)) != 0) {
        return false;
    }

    // 16-bit short UUID lives in bytes [12..13] (little-endian) for this custom base.
    out = static_cast<uint16_t>((static_cast<uint16_t>(raw[13]) << 8) | raw[12]);
    return true;
}

// Get the 16-bit short UUID from a NimBLE UUID (tries native conversion, then V1 fallback)
inline uint16_t shortUuid(const NimBLEUUID& uuid) {
    NimBLEUUID uuid16 = uuid;
    uuid16.to16();
    if (uuid16.bitSize() == BLE_UUID_TYPE_16) {
        const uint8_t* val = uuid16.getValue();
        if (val) {
            uint16_t out = 0;
            memcpy(&out, val, sizeof(out));
            return out;
        }
    }

    uint16_t v1Short = 0;
    if (extractV1ShortUuidFrom128(uuid, v1Short)) {
        return v1Short;
    }
    return 0;
}

// ============================================================================
// V1 reconnect failure threshold
// ============================================================================

constexpr uint8_t V1_BLE_MAX_BACKOFF_FAILURES = 5;
// Retained constant name for compatibility; it now represents the number of
// consecutive failed V1 connection sequences tolerated before a hard reset.

inline bool hitsV1BleHardResetThreshold(uint8_t consecutiveFailures) {
    return consecutiveFailures >= V1_BLE_MAX_BACKOFF_FAILURES;
}

// ============================================================================
// Debug Log Controls
// ============================================================================

// ============================================================================
// BLE client lifetime contract.
// ============================================================================
// NimBLE maintains a fixed 3-slot internal client array. Deleting a client
// at runtime (NimBLEDevice::deleteClient) corrupts the heap — the slot is
// freed but internal bookkeeping is not updated, leading to use-after-free.
//
// deleteClient() and deinit() are prohibited. deleteAllBonds() is restricted
// to fresh-flash recovery in ble_client.cpp.
// ============================================================================

// ============================================================================
// Shared State (defined in ble_client.cpp)
// ============================================================================

// Spinlock for deferring settings writes from BLE scan callbacks
extern portMUX_TYPE pendingAddrMux;
// Instance pointer for callbacks
extern V1BLEClient* instancePtr;
