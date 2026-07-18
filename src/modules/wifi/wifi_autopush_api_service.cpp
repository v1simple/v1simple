#include "wifi_autopush_api_service.h"

#include <algorithm>

#include <ArduinoJson.h>

#include "wifi_api_response.h"

namespace WifiAutoPushApiService {

void handleApiSlots(WebServer& server, const Runtime& runtime) {
    SlotsSnapshot snapshot;
    if (runtime.loadSlotsSnapshot) {
        runtime.loadSlotsSnapshot(snapshot, runtime.loadSlotsSnapshotCtx);
    }

    JsonDocument doc;
    doc["enabled"] = snapshot.enabled;
    doc["activeSlot"] = snapshot.activeSlot;

    JsonArray slots = doc["slots"].to<JsonArray>();
    for (const SlotConfig& slot : snapshot.slots) {
        JsonObject obj = slots.add<JsonObject>();
        obj["name"] = slot.name;
        obj["profile"] = slot.profile;
        obj["mode"] = slot.mode;
        obj["color"] = slot.color;
        obj["volume"] = slot.volume;
        obj["muteVolume"] = slot.muteVolume;
        obj["darkMode"] = slot.darkMode;
        obj["muteToZero"] = slot.muteToZero;
        obj["alertPersist"] = slot.alertPersist;
        obj["priorityArrowOnly"] = slot.priorityArrowOnly;
    }

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiStatus(WebServer& server, const Runtime& runtime) {
    String json;
    if (runtime.loadPushStatusJson && runtime.loadPushStatusJson(json, runtime.loadPushStatusJsonCtx)) {
        server.send(200, "application/json", json);
        return;
    }
    server.send(500, "application/json", "{\"error\":\"Push status not available\"}");
}

void handleApiSlotSave(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                       void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;

    if (!server.hasArg("slot") || !server.hasArg("profile") || !server.hasArg("mode")) {
        server.send(400, "application/json", "{\"error\":\"Missing parameters\"}");
        return;
    }

    int slot = server.arg("slot").toInt();
    String profile = server.arg("profile");
    int mode = server.arg("mode").toInt();
    String name = server.hasArg("name") ? server.arg("name") : "";
    int color = server.hasArg("color") ? server.arg("color").toInt() : -1;
    int volume = server.hasArg("volume") ? server.arg("volume").toInt() : -1;
    int muteVol = server.hasArg("muteVol") ? server.arg("muteVol").toInt() : -1;
    bool hasDarkMode = server.hasArg("darkMode");
    bool darkMode = hasDarkMode ? (server.arg("darkMode") == "true") : false;
    bool hasMuteToZero = server.hasArg("muteToZero");
    bool muteToZero = hasMuteToZero ? (server.arg("muteToZero") == "true") : false;
    bool hasAlertPersist = server.hasArg("alertPersist");
    int alertPersist = hasAlertPersist ? server.arg("alertPersist").toInt() : -1;

    if (slot < 0 || slot > 2) {
        server.send(400, "application/json", "{\"error\":\"Invalid slot\"}");
        return;
    }

    bool changed = false;

    if (runtime.applySlotUpdate) {
        SlotUpdateRequest request;
        request.slot = slot;
        request.hasName = name.length() > 0;
        request.name = name;
        request.hasColor = color >= 0;
        request.color = static_cast<uint16_t>(std::max(0, color));
        request.hasVolume = volume >= 0;
        request.volume = static_cast<uint8_t>(std::max(0, volume));
        request.hasMuteVolume = muteVol >= 0;
        request.muteVolume = static_cast<uint8_t>(std::max(0, muteVol));
        request.hasDarkMode = hasDarkMode;
        request.darkMode = darkMode;
        request.hasMuteToZero = hasMuteToZero;
        request.muteToZero = muteToZero;
        request.hasAlertPersist = hasAlertPersist && alertPersist >= 0;
        request.alertPersist = static_cast<uint8_t>(std::max(0, std::min(5, alertPersist)));
        request.hasPriorityArrowOnly = server.hasArg("priorityArrowOnly");
        request.priorityArrowOnly = server.arg("priorityArrowOnly") == "true";
        request.profile = profile;
        request.mode = mode;
        changed = runtime.applySlotUpdate(request, runtime.applySlotUpdateCtx);
    } else {
        if (name.length() > 0 && runtime.setSlotName) {
            runtime.setSlotName(slot, name, runtime.setSlotNameCtx);
            changed = true;
        }

        if (color >= 0 && runtime.setSlotColor) {
            runtime.setSlotColor(slot, static_cast<uint16_t>(color), runtime.setSlotColorCtx);
            changed = true;
        }

        uint8_t existingVol = runtime.getSlotVolume ? runtime.getSlotVolume(slot, runtime.getSlotVolumeCtx) : 0;
        uint8_t existingMute =
            runtime.getSlotMuteVolume ? runtime.getSlotMuteVolume(slot, runtime.getSlotMuteVolumeCtx) : 0;
        uint8_t vol = (volume >= 0) ? static_cast<uint8_t>(volume) : existingVol;
        uint8_t mute = (muteVol >= 0) ? static_cast<uint8_t>(muteVol) : existingMute;

        if ((volume >= 0 || muteVol >= 0) && runtime.setSlotVolumes) {
            runtime.setSlotVolumes(slot, vol, mute, runtime.setSlotVolumesCtx);
            changed = true;
        }

        if (hasDarkMode && runtime.setSlotDarkMode) {
            runtime.setSlotDarkMode(slot, darkMode, runtime.setSlotDarkModeCtx);
            changed = true;
        }
        if (hasMuteToZero && runtime.setSlotMuteToZero) {
            runtime.setSlotMuteToZero(slot, muteToZero, runtime.setSlotMuteToZeroCtx);
            changed = true;
        }

        if (hasAlertPersist && alertPersist >= 0 && runtime.setSlotAlertPersistSec) {
            int clamped = std::max(0, std::min(5, alertPersist));
            runtime.setSlotAlertPersistSec(slot, static_cast<uint8_t>(clamped), runtime.setSlotAlertPersistSecCtx);
            changed = true;
        }

        if (server.hasArg("priorityArrowOnly") && runtime.setSlotPriorityArrowOnly) {
            bool prioArrow = server.arg("priorityArrowOnly") == "true";
            runtime.setSlotPriorityArrowOnly(slot, prioArrow, runtime.setSlotPriorityArrowOnlyCtx);
            changed = true;
        }

        if (runtime.setSlotProfileAndMode) {
            runtime.setSlotProfileAndMode(slot, profile, mode, runtime.setSlotProfileAndModeCtx);
            changed = true;
        }
    }

    if (changed && runtime.getActiveSlot && runtime.drawProfileIndicator &&
        slot == runtime.getActiveSlot(runtime.getActiveSlotCtx)) {
        runtime.drawProfileIndicator(slot, runtime.drawProfileIndicatorCtx);
    }

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiActivate(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                       void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;

    if (!server.hasArg("slot")) {
        server.send(400, "application/json", "{\"error\":\"Missing slot parameter\"}");
        return;
    }

    int slot = server.arg("slot").toInt();
    bool enable = server.hasArg("enable") ? (server.arg("enable") == "true") : true;

    if (slot < 0 || slot > 2) {
        server.send(400, "application/json", "{\"error\":\"Invalid slot\"}");
        return;
    }

    if (runtime.applyActivation) {
        ActivationRequest request;
        request.slot = slot;
        request.enable = enable;
        runtime.applyActivation(request, runtime.applyActivationCtx);
    } else {
        if (runtime.setActiveSlot) {
            runtime.setActiveSlot(slot, runtime.setActiveSlotCtx);
        }
        if (runtime.setAutoPushEnabled) {
            runtime.setAutoPushEnabled(enable, runtime.setAutoPushEnabledCtx);
        }
    }

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiPushNow(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                      void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;

    if (runtime.maintenanceBootActive) {
        server.send(409, "application/json",
                    "{\"success\":false,\"error\":\"live_push_unavailable_in_maintenance\","
                    "\"message\":\"Live V1 push is unavailable in maintenance mode\"}");
        return;
    }

    if (!server.hasArg("slot")) {
        server.send(400, "application/json", "{\"error\":\"Missing slot parameter\"}");
        return;
    }

    int slot = server.arg("slot").toInt();
    if (slot < 0 || slot > 2) {
        server.send(400, "application/json", "{\"error\":\"Invalid slot\"}");
        return;
    }

    if (!runtime.queuePushNow) {
        server.send(500, "application/json", "{\"error\":\"Failed to load profile\"}");
        return;
    }

    PushNowRequest request;
    request.slot = slot;
    if (server.hasArg("profile") && server.arg("profile").length() > 0) {
        request.hasProfileOverride = true;
        request.profileName = server.arg("profile");
        if (server.hasArg("mode")) {
            request.hasModeOverride = true;
            request.mode = server.arg("mode").toInt();
        }
    }

    switch (runtime.queuePushNow(request, runtime.queuePushNowCtx)) {
    case PushNowQueueResult::QUEUED:
        server.send(200, "application/json", "{\"success\":true,\"queued\":true}");
        return;
    case PushNowQueueResult::V1_NOT_CONNECTED:
        server.send(503, "application/json", "{\"error\":\"V1 not connected\"}");
        return;
    case PushNowQueueResult::ALREADY_IN_PROGRESS:
        server.send(409, "application/json", "{\"error\":\"Push already in progress\"}");
        return;
    case PushNowQueueResult::NO_PROFILE_CONFIGURED:
        server.send(400, "application/json", "{\"error\":\"No profile configured for this slot\"}");
        return;
    case PushNowQueueResult::PROFILE_LOAD_FAILED:
    default:
        server.send(500, "application/json", "{\"error\":\"Failed to load profile\"}");
        return;
    }
}

} // namespace WifiAutoPushApiService
