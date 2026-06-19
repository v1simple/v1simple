#pragma once

#include <cstddef>
#include <cstdint>

#include "settings.h"
#include "obd_runtime_module.h"

class ObdSettingsSyncModule {
public:
    void begin(SettingsManager* settings, ObdRuntimeModule* obdRuntimeModule);
    void process(uint32_t nowMs);

#ifdef UNIT_TEST
    bool hasPendingSnapshotForTest() const { return pendingValid_; }
    uint32_t getPendingChangedAtMsForTest() const { return pendingChangedAtMs_; }
#endif

private:
    static constexpr size_t ADDR_BUF_LEN = 18;
    static constexpr uint32_t STABILITY_WINDOW_MS = 5000;

    struct Snapshot {
        char savedAddress[ADDR_BUF_LEN] = {};
        uint8_t savedAddrType = 0;
    };

    static void copyString(char* dest, size_t destLen, const char* src);
    static bool snapshotsEqual(const Snapshot& lhs, const Snapshot& rhs);

    Snapshot captureRuntimeSnapshot() const;
    bool settingsMatchSnapshot(const Snapshot& snapshot) const;
    void applySnapshot(const Snapshot& snapshot);

    SettingsManager* settings_ = nullptr;
    ObdRuntimeModule* obdRuntimeModule_ = nullptr;
    Snapshot pendingSnapshot_ = {};
    bool pendingValid_ = false;
    uint32_t pendingChangedAtMs_ = 0;
};
