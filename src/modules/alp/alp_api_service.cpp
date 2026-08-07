#include "alp_api_service.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "modules/alp/alp_runtime_module.h"
#include "modules/wifi/wifi_api_response.h"
#include "modules/wifi/wifi_json_document.h"

namespace AlpApiService {

namespace {

void sendMaintenanceModeError(WebServer& server) {
    WifiJson::Document doc;
    doc["error"] = "maintenance_mode";
    doc["message"] = "ALP runtime status is not available in maintenance mode";
    WifiApiResponse::sendJsonDocument(server, 409, doc);
}

void sendRuntimeUnavailableError(WebServer& server) {
    server.send(503, "application/json", "{\"error\":\"alp runtime not wired\"}");
}

} // namespace

void handleApiStatus(WebServer& server, AlpRuntimeModule* alpRuntime, void (*markUiActivity)(void* ctx),
                     void* uiActivityCtx, bool maintenanceBootActive) {
    if (markUiActivity)
        markUiActivity(uiActivityCtx);
    if (maintenanceBootActive) {
        sendMaintenanceModeError(server);
        return;
    }
    if (!alpRuntime) {
        sendRuntimeUnavailableError(server);
        return;
    }

    const AlpStatus s = alpRuntime->snapshot();
    const AlertSession& session = alpRuntime->currentSession();
    const uint32_t nowMs = millis();

    WifiJson::Document doc;

    // ── Module-level status ──────────────────────────────────────────
    doc["enabled"] = alpRuntime->isEnabled();
    doc["state"] = static_cast<int>(s.state);
    doc["stateName"] = alpStateName(s.state);
    doc["uartActive"] = s.uartActive;
    doc["ownsLaserDisplay"] = alpRuntime->ownsLaserDisplay();

    // ── Last identified gun (persistent across alerts) ───────────────
    JsonObject lastGun = doc["lastGun"].to<JsonObject>();
    lastGun["type"] = static_cast<int>(s.lastGun);
    lastGun["name"] = alpGunName(s.lastGun);
    lastGun["abbrev"] = alpGunAbbrev(s.lastGun);
    lastGun["identifiedMs"] = s.lastGunTimestampMs;
    lastGun["ageMs"] = (s.lastGunTimestampMs == 0) ? 0 : static_cast<uint32_t>(nowMs - s.lastGunTimestampMs);

    // ── V1-shape display projection (what the display consumes) ──────
    JsonObject event = doc["event"].to<JsonObject>();
    const AlpLaserEvent currentEvent = alpRuntime->currentEvent();
    event["hasLaserEvent"] = currentEvent.active;
    event["isLaserDetecting"] = currentEvent.active;
    const AlpGunType eventGun = currentEvent.gun;
    const AlpLaserDirection eventDirection = currentEvent.direction;
    event["gunType"] = static_cast<int>(eventGun);
    event["gunName"] = alpGunName(eventGun);
    event["gunAbbrev"] = alpGunAbbrev(eventGun);
    event["directionType"] = static_cast<int>(eventDirection);
    event["direction"] = alpLaserDirectionName(eventDirection);
    event["directionSampleByte1"] = currentEvent.active ? s.directionSampleByte1 : 0;

    // ── Current session detail ───────────────────────────────────────
    JsonObject sess = doc["session"].to<JsonObject>();
    sess["active"] = session.active;
    sess["isWarmUp"] = session.isWarmUp;
    sess["startMs"] = session.startMs;
    sess["endMs"] = session.endMs;
    sess["gunType"] = static_cast<int>(session.gun);
    sess["gunName"] = alpGunName(session.gun);
    sess["gunAbbrev"] = alpGunAbbrev(session.gun);
    sess["gunIdentifiedMs"] = session.gunIdentifiedMs;
    sess["triggerCount"] = session.triggerCount;
    sess["rearmCount"] = session.rearmCount;
    sess["modeAtOpen"] = session.modeAtOpen;
    sess["directionType"] = static_cast<int>(session.direction);
    sess["direction"] = alpLaserDirectionName(session.direction);
    sess["directionSampleByte1"] = session.directionSampleByte1;
    sess["isLidActive"] = (s.lastHbByte1 == 0x04);
    if (session.active && session.startMs != 0) {
        sess["durationMs"] = static_cast<uint32_t>(nowMs - session.startMs);
    } else if (!session.active && session.startMs != 0 && session.endMs != 0) {
        sess["durationMs"] = static_cast<uint32_t>(session.endMs - session.startMs);
    } else {
        sess["durationMs"] = 0;
    }

    // ── Counters ─────────────────────────────────────────────────────
    JsonObject counters = doc["counters"].to<JsonObject>();
    counters["heartbeats"] = s.heartbeatCount;
    counters["statusBursts"] = s.statusBurstCount;
    counters["frameErrors"] = s.frameErrors;
    counters["noiseWindows"] = s.noiseWindowCount;

    // ── Protocol-level diagnostics ───────────────────────────────────
    JsonObject proto = doc["protocol"].to<JsonObject>();
    proto["lastHeartbeatMs"] = s.lastHeartbeatMs;
    proto["lastHeartbeatAgeMs"] = (s.lastHeartbeatMs == 0) ? 0 : static_cast<uint32_t>(nowMs - s.lastHeartbeatMs);
    proto["lastHbByte1"] = s.lastHbByte1;

    doc["nowMs"] = nowMs;

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

} // namespace AlpApiService
