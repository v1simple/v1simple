#include "periodic_maintenance_module.h"

#include <Arduino.h>

void PeriodicMaintenanceModule::begin(const Providers& hooks) {
    providers = hooks;
    connectedPersistenceWindowAnchored_ = false;
    connectedPersistenceWindowStartedMs_ = 0;
}

void PeriodicMaintenanceModule::process(uint32_t nowMs) {
    process(nowMs, Context{});
}

void PeriodicMaintenanceModule::process(uint32_t nowMs, const Context& ctx) {
    const bool hardPressure = ctx.bleBackpressure || ctx.loopOverloaded || ctx.forceTailBleDrainPending;

    if (!ctx.bleConnected) {
        connectedPersistenceWindowAnchored_ = false;
        connectedPersistenceWindowStartedMs_ = 0;
    } else if (!connectedPersistenceWindowAnchored_) {
        // Keep the anchor bit separate from the timestamp so a connection
        // first observed at millis()==0 cannot look immediately overdue.
        connectedPersistenceWindowAnchored_ = true;
        connectedPersistenceWindowStartedMs_ = nowMs;
    }

    const bool connectedPersistenceDue =
        connectedPersistenceWindowAnchored_ &&
        static_cast<uint32_t>(nowMs - connectedPersistenceWindowStartedMs_) >= kConnectedPersistenceDeferralMs;
    const bool admitDeferredPersistence = !hardPressure && (!ctx.bleConnected || connectedPersistenceDue);
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

    // Lower-priority persistence/log-save work may touch NVS/SD. Hard loop
    // pressure always blocks it. During a BLE connection, admit one pass per
    // bounded window so connect-time dirty state cannot starve for the drive.
    if (providers.runDeferredSettingsPersist && admitDeferredPersistence) {
        providers.runDeferredSettingsPersist(providers.deferredSettingsPersistContext, nowMs);
    }

    if (providers.runDeferredSettingsBackup && admitDeferredPersistence) {
        providers.runDeferredSettingsBackup(providers.deferredSettingsBackupContext, nowMs);
    }

    // Bond service only snapshots and enqueues work for the background writer,
    // so it is safe on any unpressured tick, including connected drive time.
    if (providers.runDeferredBleBondBackup && !hardPressure) {
        providers.runDeferredBleBondBackup(providers.deferredBleBondBackupContext, nowMs);
    }

    if (providers.runStoreSave && admitDeferredPersistence) {
        providers.runStoreSave(providers.storeSaveContext, nowMs);
    }

    if (ctx.bleConnected && admitDeferredPersistence) {
        // Rearm only after a safe admission. If pressure blocks a due window,
        // it stays due and is serviced on the next unpressured tick.
        connectedPersistenceWindowStartedMs_ = nowMs;
    }
}
