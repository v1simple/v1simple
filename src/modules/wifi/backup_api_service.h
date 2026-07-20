#pragma once

#include <WebServer.h>

#include "backup_snapshot_cache.h"

namespace BackupApiService {

struct BackupNowRuntime {
    bool (*isStorageReady)(void* ctx);
    void* isStorageReadyCtx;
    bool (*isSDCard)(void* ctx);
    void* isSDCardCtx;
    bool (*backupToSD)(void* ctx);
    void* backupToSDCtx;
};

inline void sendBackupNowResponse(WebServer& server, const BackupNowRuntime& runtime) {
    const bool storageReady = runtime.isStorageReady && runtime.isStorageReady(runtime.isStorageReadyCtx);
    const bool sdCard = runtime.isSDCard && runtime.isSDCard(runtime.isSDCardCtx);
    if (!storageReady || !sdCard) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"SD card unavailable\"}");
        return;
    }

    const bool backupOk = runtime.backupToSD && runtime.backupToSD(runtime.backupToSDCtx);
    if (!backupOk) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"Backup write failed\"}");
        return;
    }

    server.send(200, "application/json", "{\"success\":true,\"message\":\"Backup written to SD\"}");
}

/// Injected dependencies for all backup/restore handlers.
/// Follows the C function-pointer + shared void* ctx pattern
/// (see wifi_settings_api_service.h for the reference design).
struct BackupRuntime {
    // GET /api/settings/backup — sendBackup dependencies
    uint32_t (*getBackupRevision)(void* ctx) = nullptr;
    uint32_t (*getCatalogRevision)(void* ctx) = nullptr;
    BackupSnapshotBuildFn buildDocument = nullptr;

    // POST /api/settings/backup-now — handleBackupNow dependencies
    bool (*isStorageReady)(void* ctx) = nullptr;
    bool (*isSDCard)(void* ctx) = nullptr;
    bool (*backupToSD)(void* ctx) = nullptr;

    // POST /api/settings/restore — handleRestore dependencies
    /// Apply a backup document. Returns true on success; sets profilesRestored.
    bool (*applyBackup)(const JsonDocument& doc, bool fullRestore, int& profilesRestored, void* ctx) = nullptr;
    /// Post-restore runtime sync (OBD, speed source, etc.).
    void (*syncAfterRestore)(void* ctx) = nullptr;

    void* ctx = nullptr;
};

/// Feeds the ESP-IDF task watchdog from the HTTP restore path.
///
/// The signature intentionally matches SettingsRestoreWatchdog::feed (settings.h)
/// so wifi_runtimes.cpp can hand it straight to applyBackupDocument() without
/// this header having to depend on settings.h. esp_task_wdt_reset() is ESP-IDF
/// only, so this compiles to a no-op on host/native builds.
void feedTaskWatchdog(void* ctx);

/// GET /api/settings/backup handler with route-level UI activity callback.
void handleApiBackup(WebServer& server, BackupSnapshotCache& cachedSnapshot, const BackupRuntime& runtime,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx, uint32_t (*millisFn)(void* ctx) = nullptr,
                     void* millisCtx = nullptr);

/// POST /api/settings/backup-now handler to force a backup to SD.
void handleApiBackupNow(WebServer& server, const BackupRuntime& runtime, bool (*checkRateLimit)(void* ctx),
                        void* rateLimitCtx, void (*markUiActivity)(void* ctx), void* uiActivityCtx);

/// POST /api/settings/restore handler with route-level policy callbacks.
void handleApiRestore(WebServer& server, const BackupRuntime& runtime, bool (*checkRateLimit)(void* ctx),
                      void* rateLimitCtx, void (*markUiActivity)(void* ctx), void* uiActivityCtx);

} // namespace BackupApiService
