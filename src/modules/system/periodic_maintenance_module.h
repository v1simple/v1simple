#pragma once

#include <stdint.h>

// Coordinates loop-tail periodic maintenance actions while preserving call order.
class PeriodicMaintenanceModule {
  public:
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
};
