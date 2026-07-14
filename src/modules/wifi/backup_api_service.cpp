#include "backup_api_service.h"

#include <ArduinoJson.h>

#include "../../backup_payload_builder.h"
#include "json_stream_response.h"
#include "wifi_json_document.h"

namespace BackupApiService {

static void sendBackup(WebServer& server, BackupSnapshotCache& cachedSnapshot, const BackupRuntime& runtime,
                       uint32_t (*millisFn)(void* ctx), void* millisCtx) {
    Serial.println("[HTTP] GET /api/settings/backup");
    server.sendHeader("Content-Disposition", "attachment; filename=\"v1simple_backup.json\"");
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

static void handleRestore(WebServer& server, const BackupRuntime& runtime) {
    Serial.println("[HTTP] POST /api/settings/restore");
    static constexpr size_t kMaxRestoreBodyBytes = 128 * 1024;

    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"No JSON body provided\"}");
        return;
    }

    // Arduino WebServer buffers the request body before dispatching this
    // handler. This remains a semantic/application cap, not a pre-allocation
    // transport cap.
    String body = server.arg("plain");
    if (body.length() > kMaxRestoreBodyBytes) {
        server.send(413, "application/json", "{\"success\":false,\"error\":\"Body too large\"}");
        return;
    }
    WifiJson::Document doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        Serial.printf("[Settings] Restore parse error: %s\n", err.c_str());
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
        return;
    }

    // Verify backup format
    if (!doc["_type"].is<const char*>() ||
        !BackupPayloadBuilder::isRecognizedBackupType(doc["_type"].as<const char*>())) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid backup format\"}");
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

    // Build response with profile count
    String response = "{\"success\":true,\"message\":\"Settings restored successfully";
    if (profilesRestored > 0) {
        response += " (" + String(profilesRestored) + " profiles)";
    }
    response += "\"}";
    server.send(200, "application/json", response);
}

void handleApiRestore(WebServer& server, const BackupRuntime& runtime, bool (*checkRateLimit)(void* ctx),
                      void* rateLimitCtx, void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleRestore(server, runtime);
}

} // namespace BackupApiService
