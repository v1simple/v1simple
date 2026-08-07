#pragma once

#include <stdint.h>

// Coordinates loop-tail periodic maintenance actions while preserving call order.
class PeriodicMaintenanceModule {
  public:
    // Connected drive-time persistence is admitted at this cadence so dirty
    // state cannot starve for an entire ignition cycle. Hard loop pressure
    // still blocks the admission until a safe tick.
    static constexpr uint32_t kConnectedPersistenceDeferralMs = 10000;

    struct Context {
        bool bleConnected = false;
        bool bleBackpressure = false;
        bool loopOverloaded = false;
        bool forceTailBleDrainPending = false;
    };

    struct Providers {
        uint32_t (*timestampUs)(void* ctx) = nullptr;
        void* timestampContext = nullptr;

        void (*runPerfReport)(void* ctx) = nullptr;
        void* perfReportContext = nullptr;
        void (*recordPerfReportUs)(void* ctx, uint32_t elapsedUs) = nullptr;
        void* perfReportRecordContext = nullptr;

        void (*runObdSettingsSync)(void* ctx, uint32_t nowMs) = nullptr;
        void* obdSettingsSyncContext = nullptr;

        void (*runDeferredSettingsPersist)(void* ctx, uint32_t nowMs) = nullptr;
        void* deferredSettingsPersistContext = nullptr;

        void (*runDeferredSettingsBackup)(void* ctx, uint32_t nowMs) = nullptr;
        void* deferredSettingsBackupContext = nullptr;

        void (*runDeferredBleBondBackup)(void* ctx, uint32_t nowMs) = nullptr;
        void* deferredBleBondBackupContext = nullptr;

        void (*runStoreSave)(void* ctx, uint32_t nowMs) = nullptr;
        void* storeSaveContext = nullptr;
    };

    void begin(const Providers& hooks);
    void process(uint32_t nowMs);
    void process(uint32_t nowMs, const Context& ctx);

  private:
    Providers providers{};
    bool connectedPersistenceWindowAnchored_ = false;
    uint32_t connectedPersistenceWindowStartedMs_ = 0;
};
