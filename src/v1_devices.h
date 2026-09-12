/**
 * V1 Device Store
 * Persists known V1 addresses, optional friendly names, and per-device
 * default auto-push profile slot selection.
 */

#pragma once

#include <Arduino.h>
#include <FS.h>

#include <array>
#include <cstdint>
#include <vector>

struct V1DetectorSnapshot {
    bool available = false;
    uint32_t capturedBootId = 0;
    uint32_t capturedUptimeMs = 0;
    uint32_t sessionGeneration = 0;
    bool captureTimedOut = false;

    bool hasFirmwareVersion = false;
    uint32_t firmwareVersion = 0;
    bool hasUserBytes = false;
    std::array<uint8_t, 6> userBytes{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    bool hasMode = false;
    char mode = 0;
    bool hasDisplayOn = false;
    bool displayOn = true;
    bool hasCurrentVolume = false;
    uint8_t currentMainVolume = 0;
    uint8_t currentMutedVolume = 0;
    bool hasSavedVolume = false;
    uint8_t savedMainVolume = 0;
    uint8_t savedMutedVolume = 0;
};

struct V1DeviceRecord {
    String address;
    String name;
    uint8_t defaultProfile = 0; // 0=none/global slot, 1..3=auto-push slot override
    uint32_t lastSeenMs = 0; // Informational uptime; durable list order owns recency.
    V1DetectorSnapshot snapshot;
};

// Normalize BLE address to canonical upper-case AA:BB:CC:DD:EE:FF.
// Returns empty string when invalid.
String normalizeV1DeviceAddress(const String& rawAddress);

class V1DeviceStore {
  public:
    V1DeviceStore();

    bool begin(fs::FS* filesystem, fs::FS* importFilesystem = nullptr);
    bool isReady() const { return ready_; }

    std::vector<V1DeviceRecord> listDevices() const;

    bool upsertDevice(const String& address);
    // Historical hints seed only a missing catalog. A connection recorded
    // during a storage outage may also augment an existing catalog.
    bool bootstrapDevice(const String& address, bool fromDegradedConnection);
    bool touchDeviceInMemory(const String& address);
    // Record one pre-apply settings observation for this detector. Persistence
    // is deliberately deferred through the existing device-store writer.
    bool recordSnapshotInMemory(const String& address, const V1DetectorSnapshot& snapshot);
    bool setDeviceName(const String& address, const String& name);
    bool setDeviceDefaultProfile(const String& address, uint8_t defaultProfile);
    bool removeDevice(const String& address);
    bool hasPendingSave() const { return dirty_ || mirrorDirty_; }
    bool flushPendingSave();

    uint8_t getDeviceDefaultProfile(const String& address) const;
    bool getLatestSnapshot(V1DeviceRecord& device) const;

  private:
    static constexpr size_t MAX_DEVICES = 16;
    static constexpr size_t MAX_NAME_LEN = 32;
    static constexpr size_t MAX_STORE_BYTES = 12288;

    enum class StoreReadStatus : uint8_t { Missing = 0, Valid, Invalid };

    struct StoreSnapshot {
        StoreReadStatus status = StoreReadStatus::Missing;
        uint32_t generation = 0;
        uint32_t contentCrc = 0;
        bool legacy = false;
        bool needsRewrite = false;
        bool loadedFromRollback = false;
        std::vector<V1DeviceRecord> devices;
    };

    fs::FS* fs_ = nullptr;
    fs::FS* secondaryFs_ = nullptr;
    bool ready_ = false;
    uint32_t generation_ = 0;
    std::vector<V1DeviceRecord> devices_;

    bool loadFromStore();
    bool saveToStore();
    StoreSnapshot readStore(fs::FS& filesystem) const;
    bool writeStore(fs::FS& filesystem, uint32_t generation, bool preserveValidRollback = false) const;
    bool reconcileStores();

    bool migrateLegacyFiles(fs::FS* sourceFs);

    static String sanitizeName(const String& raw);
    static uint8_t clampDefaultProfileValue(int raw);
    bool persistDirtyStore();
    bool upsertDeviceInternal(const String& address, bool persistNow);

    int findDeviceIndex(const String& normalizedAddress) const;
    void trimToCapacity();

    bool dirty_ = false;
    bool mirrorDirty_ = false;
};
