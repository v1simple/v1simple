#include "wifi_autopush_api_service.h"

#include <algorithm>

#include <ArduinoJson.h>

#include "wifi_api_response.h"
#include "wifi_json_document.h"
#include "profile_name.h"

namespace WifiAutoPushApiService {

void handleApiSlots(WebServer& server, const Runtime& runtime) {
    SlotsSnapshot snapshot;
    if (runtime.loadSlotsSnapshot) {
        runtime.loadSlotsSnapshot(snapshot, runtime.loadSlotsSnapshotCtx);
    }

    WifiJson::Document doc;
    doc["enabled"] = snapshot.enabled;
    doc["activeSlot"] = snapshot.activeSlot;

    JsonArray slots = doc["slots"].to<JsonArray>();
    for (const SlotConfig& slot : snapshot.slots) {
        JsonObject obj = slots.add<JsonObject>();
        obj["name"] = slot.name;
        obj["profile"] = slot.profile;
        obj["mode"] = slot.mode;
        obj["color"] = slot.color;
        obj["volumeConfigured"] = slot.volumeConfigured;
        obj["volume"] = slot.volumeConfigured ? slot.volume : 0;
        obj["muteVolume"] = slot.volumeConfigured ? slot.muteVolume : 0;
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
    const bool hasVolumeConfigured = server.hasArg("volumeConfigured");
    const bool volumeConfigured = hasVolumeConfigured && server.arg("volumeConfigured") == "true";
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

    if (profile.length() > 0) {
        String canonicalProfile;
        const ProfileNameStatus nameStatus = canonicalizeProfileName(profile, canonicalProfile);
        if (nameStatus != ProfileNameStatus::Valid) {
            server.send(400, "application/json", String("{\"error\":\"") + profileNameStatusMessage(nameStatus) +
                                                     "\"}");
            return;
        }
        profile = canonicalProfile;
        if (runtime.validateProfileAssignment) {
            const ProfileAssignmentStatus assignment =
                runtime.validateProfileAssignment(profile, runtime.validateProfileAssignmentCtx);
            if (assignment != ProfileAssignmentStatus::Success) {
                const int code = assignment == ProfileAssignmentStatus::Busy ? 409 :
                                 assignment == ProfileAssignmentStatus::NotFound ? 400 : 500;
                const char* error = assignment == ProfileAssignmentStatus::Busy ? "Profile storage busy; retry" :
                                    assignment == ProfileAssignmentStatus::NotFound ? "Profile does not exist" :
                                    assignment == ProfileAssignmentStatus::Corrupt ? "Profile is corrupt" :
                                    assignment == ProfileAssignmentStatus::InvalidName ? "Invalid profile name" :
                                    "Profile could not be read";
                server.send(code, "application/json", String("{\"error\":\"") + error + "\"}");
                return;
            }
        }
    }

    if (hasVolumeConfigured) {
        if (volumeConfigured && (!server.hasArg("volume") || !server.hasArg("muteVol") || volume < 0 || volume > 9 ||
                                 muteVol < 0 || muteVol > 9)) {
            server.send(400, "application/json", "{\"error\":\"Both main and mute volume must be between 0 and 9\"}");
            return;
        }
    } else if (server.hasArg("volume") != server.hasArg("muteVol") ||
               ((server.hasArg("volume") || server.hasArg("muteVol")) &&
                (volume < 0 || volume > 9 || muteVol < 0 || muteVol > 9))) {
        server.send(400, "application/json", "{\"error\":\"Main and mute volume must be configured together\"}");
        return;
    }

    bool persisted = false;

    if (runtime.applySlotUpdate) {
        SlotUpdateRequest request;
        request.slot = slot;
        request.hasName = name.length() > 0;
        request.name = name;
        request.hasColor = color >= 0;
        request.color = static_cast<uint16_t>(std::max(0, color));
        request.hasVolumeConfigured = hasVolumeConfigured;
        request.volumeConfigured = volumeConfigured;
        request.hasVolume = hasVolumeConfigured || volume >= 0;
        request.volume = volumeConfigured || !hasVolumeConfigured ? static_cast<uint8_t>(std::max(0, volume)) : 0xFF;
        request.hasMuteVolume = hasVolumeConfigured || muteVol >= 0;
        request.muteVolume =
            volumeConfigured || !hasVolumeConfigured ? static_cast<uint8_t>(std::max(0, muteVol)) : 0xFF;
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
        persisted = runtime.applySlotUpdate(request, runtime.applySlotUpdateCtx);
    } else {
        if (name.length() > 0 && runtime.setSlotName) {
            runtime.setSlotName(slot, name, runtime.setSlotNameCtx);
            persisted = true;
        }

        if (color >= 0 && runtime.setSlotColor) {
            runtime.setSlotColor(slot, static_cast<uint16_t>(color), runtime.setSlotColorCtx);
            persisted = true;
        }

        uint8_t existingVol = runtime.getSlotVolume ? runtime.getSlotVolume(slot, runtime.getSlotVolumeCtx) : 0;
        uint8_t existingMute =
            runtime.getSlotMuteVolume ? runtime.getSlotMuteVolume(slot, runtime.getSlotMuteVolumeCtx) : 0;
        uint8_t vol = hasVolumeConfigured && !volumeConfigured
                          ? 0xFF
                          : ((volume >= 0) ? static_cast<uint8_t>(volume) : existingVol);
        uint8_t mute = hasVolumeConfigured && !volumeConfigured
                           ? 0xFF
                           : ((muteVol >= 0) ? static_cast<uint8_t>(muteVol) : existingMute);

        if ((hasVolumeConfigured || volume >= 0 || muteVol >= 0) && runtime.setSlotVolumes) {
            runtime.setSlotVolumes(slot, vol, mute, runtime.setSlotVolumesCtx);
            persisted = true;
        }

        if (hasDarkMode && runtime.setSlotDarkMode) {
            runtime.setSlotDarkMode(slot, darkMode, runtime.setSlotDarkModeCtx);
            persisted = true;
        }
        if (hasMuteToZero && runtime.setSlotMuteToZero) {
            runtime.setSlotMuteToZero(slot, muteToZero, runtime.setSlotMuteToZeroCtx);
            persisted = true;
        }

        if (hasAlertPersist && alertPersist >= 0 && runtime.setSlotAlertPersistSec) {
            int clamped = std::max(0, std::min(5, alertPersist));
            runtime.setSlotAlertPersistSec(slot, static_cast<uint8_t>(clamped), runtime.setSlotAlertPersistSecCtx);
            persisted = true;
        }

        if (server.hasArg("priorityArrowOnly") && runtime.setSlotPriorityArrowOnly) {
            bool prioArrow = server.arg("priorityArrowOnly") == "true";
            runtime.setSlotPriorityArrowOnly(slot, prioArrow, runtime.setSlotPriorityArrowOnlyCtx);
            persisted = true;
        }

        if (runtime.setSlotProfileAndMode) {
            runtime.setSlotProfileAndMode(slot, profile, mode, runtime.setSlotProfileAndModeCtx);
            persisted = true;
        }
    }

    if (!persisted) {
        server.send(500, "application/json", "{\"error\":\"Slot persistence failed\"}");
        return;
    }

    if (runtime.getActiveSlot && runtime.drawProfileIndicator &&
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

} // namespace WifiAutoPushApiService
