#include "../../../src/modules/wifi/backup_api_service.h"

namespace BackupApiService {

// Internal implementation entrypoints defined in backup_api_service.cpp.
void sendBackup(WebServer& server,
                BackupSnapshotCache& cachedSnapshot,
                const BackupRuntime& runtime,
                uint32_t (*millisFn)(void* ctx), void* millisCtx);
void handleBackupNow(WebServer& server, const BackupRuntime& runtime);
void handleRestore(WebServer& server, const BackupRuntime& runtime);

void handleApiBackup(WebServer& server,
                     BackupSnapshotCache& cachedSnapshot,
                     const BackupRuntime& runtime,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx,
                     uint32_t (*millisFn)(void* ctx), void* millisCtx) {
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    sendBackup(server, cachedSnapshot, runtime, millisFn, millisCtx);
}

void handleApiBackupNow(WebServer& server,
                        const BackupRuntime& runtime,
                        bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                        void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleBackupNow(server, runtime);
}

void handleApiRestore(WebServer& server,
                      const BackupRuntime& runtime,
                      bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                      void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleRestore(server, runtime);
}

}  // namespace BackupApiService
