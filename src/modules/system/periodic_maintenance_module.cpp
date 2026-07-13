#include "periodic_maintenance_module.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#if PERF_METRICS
static uint32_t sLastHeapIntegrityCheckMs = 0;
static constexpr uint32_t HEAP_INTEGRITY_CHECK_INTERVAL_MS = 30000;  // Every 30 seconds
#endif

void PeriodicMaintenanceModule::begin(const Providers& hooks) {
    providers = hooks;
}

void PeriodicMaintenanceModule::process(uint32_t nowMs) {
    process(nowMs, Context{});
}

void PeriodicMaintenanceModule::process(uint32_t nowMs, const Context& ctx) {
    const bool deferLowPriorityMaintenance =
        ctx.bleConnected || ctx.bleBackpressure || ctx.loopOverloaded ||
        ctx.forceTailBleDrainPending;
#if PERF_METRICS
    // Periodic heap integrity check (every ~30 seconds).
    // heap_caps_check_integrity_all(true) can take hundreds of milliseconds on
    // real hardware, so never run it while V1/BLE/display work is active or
    // while the loop is already pressured. This diagnostic is useful on the
    // bench, but it is lower priority than every drive-time tier.
    if (!deferLowPriorityMaintenance &&
        (sLastHeapIntegrityCheckMs == 0 ||
         (nowMs - sLastHeapIntegrityCheckMs >= HEAP_INTEGRITY_CHECK_INTERVAL_MS))) {
        sLastHeapIntegrityCheckMs = nowMs;

        if (!heap_caps_check_integrity_all(true)) {
            Serial.println("[HEAP] !!! INTEGRITY CHECK FAILED !!!");
        }
    }
#endif
    if (providers.runPerfReport) {
        uint32_t startUs = 0;
        if (providers.timestampUs) {
            startUs = providers.timestampUs(providers.timestampContext);
        }

        providers.runPerfReport(providers.perfReportContext);

        if (providers.recordPerfReportUs && providers.timestampUs) {
            const uint32_t elapsedUs =
                static_cast<uint32_t>(providers.timestampUs(providers.timestampContext) - startUs);
            providers.recordPerfReportUs(providers.perfReportRecordContext, elapsedUs);
        }
    }

    if (providers.runObdSettingsSync) {
        providers.runObdSettingsSync(providers.obdSettingsSyncContext, nowMs);
    }

    // Lower-priority persistence/log-save work may touch NVS/SD. Keep the
    // atomic writers, but never admit them during connected drive, pressured
    // loops, or immediately before a forced tail BLE drain.
    if (providers.runDeferredSettingsPersist && !deferLowPriorityMaintenance) {
        providers.runDeferredSettingsPersist(providers.deferredSettingsPersistContext, nowMs);
    }

    if (providers.runDeferredSettingsBackup && !deferLowPriorityMaintenance) {
        providers.runDeferredSettingsBackup(providers.deferredSettingsBackupContext, nowMs);
    }

    if (providers.runDeferredBleBondBackup && !deferLowPriorityMaintenance) {
        providers.runDeferredBleBondBackup(providers.deferredBleBondBackupContext, nowMs);
    }

    if (providers.runStoreSave && !deferLowPriorityMaintenance) {
        providers.runStoreSave(providers.storeSaveContext, nowMs);
    }
}
