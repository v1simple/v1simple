#pragma once

#include <ArduinoJson.h>
#include <WebServer.h>

#include <cstddef>
#include <cstdint>

namespace BackupApiService {

struct BackupSnapshotCache {
    char* data = nullptr;
    size_t capacity = 0;
    size_t length = 0;
    bool inPsram = false;
    uint32_t snapshotMs = 0;
    uint32_t settingsRevision = 0;
    uint32_t profileRevision = 0;
    bool valid = false;
};

using BackupSnapshotBuildFn = void (*)(JsonDocument&, uint32_t snapshotMs, void* ctx);

bool sendCachedBackupSnapshot(WebServer& server,
                              BackupSnapshotCache& cache,
                              uint32_t settingsRevision,
                              uint32_t profileRevision,
                              BackupSnapshotBuildFn buildSnapshot,
                              void* buildCtx,
                              uint32_t (*millisFn)(void* ctx) = nullptr,
                              void* millisCtx = nullptr);

void releaseBackupSnapshotCache(BackupSnapshotCache& cache);

}  // namespace BackupApiService
