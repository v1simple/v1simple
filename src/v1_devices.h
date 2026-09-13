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
#include "packet_parser_types.h"

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
    bool hasBluetoothIndicator = false;
    V1BluetoothIndicatorState bluetoothIndicator = V1BluetoothIndicatorState::Invalid;
    bool hasCurrentVolume = false;
    uint8_t currentMainVolume = 0;
    uint8_t currentMutedVolume = 0;
    bool hasSavedVolume = false;
    uint8_t savedMainVolume = 0;
    uint8_t savedMutedVolume = 0;
    bool hasSweepSections = false;
    uint8_t sweepSectionCount = 0;
    std::array<V1SweepSectionObservation, 15> sweepSections{};
    bool hasMaxSweepIndex = false;
    uint8_t maxSweepIndex = 0;
    bool hasSweepDefinitions = false;
    std::array<V1SweepDefinitionObservation, 64> sweepDefinitions{};
};

struct V1DeviceRecord {
    String address;
    String name;
    uint8_t defaultProfile = 0; // 0=none/global slot, 1..3=auto-push slot override
    uint32_t lastSeenMs = 0; // Informational uptime; durable list order owns recency.
    V1DetectorSnapshot snapshot;
};

enum class V1DeviceMutationStatus : uint8_t {
    Invalid = 0,
    Busy,
    Unavailable,
    NotCommitted,
    DurablePending,
    PrimaryCommittedMirrorPending,
    FullyMirrored,
};

struct V1DeviceMutationResult {
    V1DeviceMutationStatus status = V1DeviceMutationStatus::NotCommitted;

    bool committed() const {
        return status == V1DeviceMutationStatus::PrimaryCommittedMirrorPending ||
               status == V1DeviceMutationStatus::FullyMirrored;
    }
    bool fullyMirrored() const { return status == V1DeviceMutationStatus::FullyMirrored; }
    explicit operator bool() const { return committed(); }
};

enum class V1DeviceDefaultProfileStatus : uint8_t {
    Found = 0,
    NoOverride,
    Unavailable,
};

struct V1DeviceDefaultProfileResult {
    V1DeviceDefaultProfileStatus status = V1DeviceDefaultProfileStatus::Unavailable;
    uint8_t profile = 0;
};

enum class V1DeviceSnapshotStatus : uint8_t {
    Found = 0,
    NotFound,
    Unavailable,
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
    bool listDevicesChecked(std::vector<V1DeviceRecord>& output) const;
    bool catalogReadable() const;
    bool containsDeviceChecked(const String& address, bool& present) const;

    bool upsertDevice(const String& address);
    // Historical hints seed only a missing catalog. A connection recorded
    // during a storage outage may also augment an existing catalog.
    bool bootstrapDevice(const String& address, bool fromDegradedConnection);
    bool touchDeviceInMemory(const String& address);
    // Record one pre-apply settings observation for this detector. Persistence
    // is deliberately deferred through the existing device-store writer.
    bool recordSnapshotInMemory(const String& address, const V1DetectorSnapshot& snapshot);
    V1DeviceMutationResult setDeviceName(const String& address, const String& name);
    V1DeviceMutationResult setDeviceDefaultProfile(const String& address, uint8_t defaultProfile);
    V1DeviceMutationResult removeDevice(const String& address);
    bool hasPendingSave() const { return dirty_ || mirrorDirty_; }
    bool flushPendingSave();

    V1DeviceDefaultProfileResult getDeviceDefaultProfileChecked(const String& address) const;
    uint8_t getDeviceDefaultProfile(const String& address) const;
    bool getLatestSnapshot(V1DeviceRecord& device) const;
    V1DeviceSnapshotStatus getSnapshotForAddressChecked(const String& address,
                                                        V1DeviceRecord& device) const;

  private:
    static constexpr size_t MAX_DEVICES = 16;
    static constexpr size_t MAX_NAME_LEN = 32;
    // Native worst-case coverage pins a 16-device catalog with every sweep
    // field and 32 quote characters in every legal name at 70,180 bytes.
    // Rounding to the next 4 KiB boundary gives 73,728 bytes and 3,548 bytes
    // of serialization/parser headroom while bounding malformed input and
    // peak duplication even when external RAM is available.
    static constexpr size_t MAX_STORE_BYTES = 72u * 1024u;

    enum class StoreReadStatus : uint8_t { Missing = 0, Valid, Unavailable, Invalid };

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
    V1DeviceMutationResult saveToStoreResult();
    StoreSnapshot readStore(fs::FS& filesystem) const;
    bool writeStore(fs::FS& filesystem, uint32_t generation, bool preserveValidRollback = false) const;
    bool reconcileStores();

    bool migrateLegacyFiles(fs::FS* sourceFs);

    static String sanitizeName(const String& raw);
    static uint8_t clampDefaultProfileValue(int raw);
    bool persistDirtyStore();
    bool buildUpsertCandidate(const String& address, const V1DetectorSnapshot* snapshot,
                              std::vector<V1DeviceRecord>& candidate) const;
    bool upsertDeviceInternal(const String& address, bool persistNow);

    int findDeviceIndex(const String& normalizedAddress) const;
    void trimToCapacity();

    bool dirty_ = false;
    bool mirrorDirty_ = false;
    mutable bool storeReadUnavailable_ = false;
    StoreReadStatus catalogStatus_ = StoreReadStatus::Missing;
};
