/**
 * V1 Device Store
 * Persists known V1 addresses, optional friendly names, and per-device
 * default auto-push profile slot selection.
 */

#pragma once

#include <Arduino.h>
#include <FS.h>

#include <cstdint>
#include <vector>

struct V1DeviceRecord {
    String address;
    String name;
    uint8_t defaultProfile = 0; // 0=none/global slot, 1..3=auto-push slot override
    uint32_t lastSeenMs = 0;
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
    bool touchDeviceInMemory(const String& address);
    bool setDeviceName(const String& address, const String& name);
    bool setDeviceDefaultProfile(const String& address, uint8_t defaultProfile);
    bool removeDevice(const String& address);
    bool hasPendingSave() const { return dirty_ || mirrorDirty_; }
    bool flushPendingSave();

    uint8_t getDeviceDefaultProfile(const String& address) const;

  private:
    static constexpr size_t MAX_DEVICES = 16;
    static constexpr size_t MAX_NAME_LEN = 32;
    static constexpr size_t MAX_STORE_BYTES = 4096;

    enum class StoreReadStatus : uint8_t { Missing = 0, Valid, Invalid };

    struct StoreSnapshot {
        StoreReadStatus status = StoreReadStatus::Missing;
        uint32_t generation = 0;
        uint32_t contentCrc = 0;
        bool legacy = false;
        bool needsRewrite = false;
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
    bool writeStore(fs::FS& filesystem, uint32_t generation) const;
    bool reconcileStores();

    bool migrateLegacyFiles(fs::FS* sourceFs);

    static String sanitizeName(const String& raw);
    static uint8_t clampDefaultProfileValue(int raw);
    bool persistDirtyStore();
    bool upsertDeviceInternal(const String& address, bool persistNow);

    int findDeviceIndex(const String& normalizedAddress) const;
    void sortAndTrim();

    bool dirty_ = false;
    bool mirrorDirty_ = false;
};
