#include "backup_api_service.h"

#include <ArduinoJson.h>

#if !defined(UNIT_TEST)
#include <esp_task_wdt.h>
#endif

#include "../../backup_payload_builder.h"
#include "../../json_exact_input.h"
#include "../../psram_json_document.h"
#include "settings_internals.h"
#include "json_stream_response.h"

namespace BackupApiService {

void feedTaskWatchdog(void* /*ctx*/) {
#if !defined(UNIT_TEST)
    (void)esp_task_wdt_reset();
#endif
}

static void sendBackup(WebServer& server, BackupSnapshotCache& cachedSnapshot, const BackupRuntime& runtime,
                       uint32_t (*millisFn)(void* ctx), void* millisCtx) {
    Serial.println("[HTTP] GET /api/settings/backup");
    sendCachedBackupSnapshot(server, cachedSnapshot, runtime.getBackupRevision(runtime.ctx),
                             runtime.getCatalogRevision(runtime.ctx), runtime.buildDocument, runtime.ctx, millisFn,
                             millisCtx);
}

static void handleBackupNow(WebServer& server, const BackupRuntime& runtime) {
    Serial.println("[HTTP] POST /api/settings/backup-now");
    sendBackupNowResponse(server, BackupNowRuntime{
                                      runtime.isStorageReady,
                                      runtime.ctx,
                                      runtime.isSDCard,
                                      runtime.ctx,
                                      runtime.backupToSD,
                                      runtime.ctx,
                                  });
}

void handleApiBackup(WebServer& server, BackupSnapshotCache& cachedSnapshot, const BackupRuntime& runtime,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx, uint32_t (*millisFn)(void* ctx),
                     void* millisCtx) {
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    sendBackup(server, cachedSnapshot, runtime, millisFn, millisCtx);
}

void handleApiBackupNow(WebServer& server, const BackupRuntime& runtime, bool (*checkRateLimit)(void* ctx),
                        void* rateLimitCtx, void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleBackupNow(server, runtime);
}

static void handleRestoreBody(WebServer& server, const BackupRuntime& runtime, const uint8_t* body,
                              const size_t bodySize) {
    Serial.println("[HTTP] POST /api/settings/restore");

    if (!body && bodySize != 0) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"No JSON body provided\"}");
        return;
    }

    // Production routes provide this exact byte span from the raw WebServer
    // reader into one PSRAM-owned buffer. Retain the cap here for direct
    // service tests and alternate callers.
    if (bodySize > kHttpBackupDocumentMaxBytes) {
        server.send(413, "application/json", "{\"success\":false,\"error\":\"Body too large\"}");
        return;
    }
    const char* bodyData = reinterpret_cast<const char*>(body);
    const ExactJsonInput::Status exact = ExactJsonInput::validate(bodyData, bodySize);
    if (exact == ExactJsonInput::Status::MemoryUnavailable) {
        server.send(503, "application/json",
                    "{\"success\":false,\"error\":\"Backup restore memory unavailable\",\"retryable\":true}");
        return;
    }
    if (exact != ExactJsonInput::Status::Ok) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
        return;
    }
    PsramJson::Document doc;
    DeserializationError err = deserializeJson(doc, bodyData, bodySize);

    if (doc.overflowed() || err == DeserializationError::NoMemory) {
        server.send(503, "application/json",
                    "{\"success\":false,\"error\":\"Backup restore memory unavailable\",\"retryable\":true}");
        return;
    }
    if (err) {
        Serial.printf("[Settings] Restore parse error: %s\n", err.c_str());
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
        return;
    }

    // Verify backup format
    const JsonVariantConst backupType = doc["_type"];
    if (!backupType.is<const char*>() ||
        !BackupPayloadBuilder::isRecognizedBackupType(backupType.as<const char*>())) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid backup format\"}");
        return;
    }
    if (!doc["_crc32"].isUnbound() &&
        (!doc["_crc32"].is<uint32_t>() ||
         doc["_crc32"].as<uint32_t>() != BackupPayloadBuilder::computeBackupCrc32(doc))) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Backup checksum mismatch\"}");
        return;
    }
    const JsonVariantConst currentVersion = doc["_version"];
    const JsonVariantConst legacyVersion = doc["version"];
    if ((!currentVersion.isUnbound() &&
         (!currentVersion.is<int>() || currentVersion.as<int>() < 1 ||
          currentVersion.as<int>() > SD_BACKUP_VERSION)) ||
        (!legacyVersion.isUnbound() &&
         (!legacyVersion.is<int>() || legacyVersion.as<int>() < 1 ||
          legacyVersion.as<int>() > SD_BACKUP_VERSION)) ||
        (!currentVersion.isUnbound() && !legacyVersion.isUnbound() &&
         currentVersion.as<int>() != legacyVersion.as<int>())) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid backup version\"}");
        return;
    }

    int profilesRestored = 0;
    const bool success = runtime.applyBackup(doc, true, profilesRestored, runtime.ctx);
    if (!success) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"Failed to persist restored settings\"}");
        return;
    }

    runtime.syncAfterRestore(runtime.ctx);

    Serial.printf("[Settings] Restored from uploaded backup (%d profiles)\n", profilesRestored);

    // Mutation has already committed, so the terminal success response must
    // not depend on heap-backed String concatenation that can fail afterward.
    char response[128];
    const int responseLength = profilesRestored > 0
                                   ? snprintf(response, sizeof(response),
                                              "{\"success\":true,\"message\":\"Settings restored successfully "
                                              "(%d profiles)\"}",
                                              profilesRestored)
                                   : snprintf(response, sizeof(response),
                                              "{\"success\":true,\"message\":\"Settings restored successfully\"}");
    if (responseLength <= 0 || static_cast<size_t>(responseLength) >= sizeof(response)) {
        // profilesRestored is bounded by the supported catalog count, so this
        // is a compile-time-format invariant rather than a runtime OOM path.
        server.send(200, "application/json", "{\"success\":true}");
        return;
    }
    server.send(200, "application/json", response);
}

void handleApiRestore(WebServer& server, const BackupRuntime& runtime, bool (*checkRateLimit)(void* ctx),
                      void* rateLimitCtx, void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"No JSON body provided\"}");
        return;
    }
    const String body = server.arg("plain");
    handleApiRestoreBody(server, runtime, reinterpret_cast<const uint8_t*>(body.c_str()), body.length(),
                         checkRateLimit, rateLimitCtx, markUiActivity, uiActivityCtx);
}

void handleApiRestoreBody(WebServer& server, const BackupRuntime& runtime, const uint8_t* body,
                          const size_t bodySize, bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                          void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleRestoreBody(server, runtime, body, bodySize);
}

} // namespace BackupApiService
