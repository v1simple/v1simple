#include "wifi_autopush_api_service.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>

#include <ArduinoJson.h>

#include "wifi_api_response.h"
#include "exact_urlencoded_form.h"
#include "json_exact_input.h"
#include "psram_json_document.h"
#include "wifi_json_document.h"
#include "profile_name.h"
#include "settings_sanitize.h"
#include "v1_profiles.h"

namespace WifiAutoPushApiService {

namespace {

struct WebServerForm {
    WebServer& server;
    bool has(const char* key) const { return server.hasArg(key); }
    bool read(const char* key, String& value, bool allowEmpty = false) const {
        if (!server.hasArg(key)) return false;
        const String& parsed = server.arg(key);
        value = parsed;
        if (value.length() != parsed.length() ||
            (value.length() != 0 && std::memcmp(value.c_str(), parsed.c_str(), value.length()) != 0)) return false;
        if ((!allowEmpty && value.length() == 0) ||
            !ExactJsonInput::validSemanticString(value.c_str(), value.length())) return false;
        return true;
    }
};

struct ExactBodyForm {
    const ExactUrlEncodedForm& form;
    bool has(const char* key) const { return form.has(key); }
    bool read(const char* key, String& value, bool allowEmpty = false) const {
        return form.read(key, value, allowEmpty);
    }
};

template <typename Form>
bool readPresentArg(const Form& form, const char* key, String& value, bool allowEmpty = false) {
    if (!form.has(key) || !form.read(key, value, allowEmpty)) return false;
    if ((!allowEmpty && value.length() == 0) ||
        !ExactJsonInput::validSemanticString(value.c_str(), value.length())) return false;
    return true;
}

template <typename Form>
bool parseIntArg(const Form& form, const char* key, int& value, bool required = false) {
    if (!form.has(key)) return !required;
    String raw;
    if (!readPresentArg(form, key, raw)) return false;
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(raw.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX) return false;
    value = static_cast<int>(parsed);
    return true;
}

template <typename Form>
bool parseBoolArg(const Form& form, const char* key, bool& value, bool required = false) {
    if (!form.has(key)) return !required;
    String raw;
    if (!readPresentArg(form, key, raw)) return false;
    if (raw == "true") value = true;
    else if (raw == "false") value = false;
    else return false;
    return true;
}

} // namespace

void handleApiSlots(WebServer& server, const Runtime& runtime) {
    SlotsSnapshot snapshot;
    if (runtime.loadSlotsSnapshotResult) {
        if (!runtime.loadSlotsSnapshotResult(snapshot, runtime.loadSlotsSnapshotResultCtx)) {
            server.send(503, "application/json", "{\"error\":\"Slot settings memory unavailable\",\"retryable\":true}");
            return;
        }
    } else if (runtime.loadSlotsSnapshot) {
        runtime.loadSlotsSnapshot(snapshot, runtime.loadSlotsSnapshotCtx);
    }

    PsramJson::Document doc;
    doc["enabled"] = snapshot.enabled;
    doc["activeSlot"] = snapshot.activeSlot;
    doc["schemaVersion"] = snapshot.profileOwned ? V1_PROFILE_SCHEMA_VERSION : 1;
    doc["detectorConfigurationOwner"] = snapshot.profileOwned ? "profile" : "legacy-slot";

    JsonArray slots = doc["slots"].to<JsonArray>();
    for (const SlotConfig& slot : snapshot.slots) {
        JsonObject obj = slots.add<JsonObject>();
        obj["name"] = slot.name;
        obj["profile"] = slot.profile;
        obj["color"] = slot.color;
        if (!snapshot.profileOwned) {
            obj["mode"] = slot.mode;
            obj["volumeConfigured"] = slot.volumeConfigured;
            obj["volume"] = slot.volumeConfigured ? slot.volume : 0;
            obj["muteVolume"] = slot.volumeConfigured ? slot.muteVolume : 0;
            obj["darkMode"] = slot.darkMode;
            obj["muteToZero"] = slot.muteToZero;
        }
        obj["alertPersist"] = slot.alertPersist;
        obj["priorityArrowOnly"] = slot.priorityArrowOnly;
    }

    const size_t responseBytes = measureJson(doc);
    if (doc.overflowed() || responseBytes == 0 || responseBytes > 4096u) {
        server.send(503, "application/json", "{\"error\":\"Slot settings memory unavailable\",\"retryable\":true}");
        return;
    }
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiStatus(WebServer& server, const Runtime& runtime) {
    if (runtime.appendPushStatusJson) {
        PsramJson::Document doc;
        if (!runtime.appendPushStatusJson(doc.to<JsonObject>(), runtime.appendPushStatusJsonCtx) ||
            doc.overflowed()) {
            server.send(503, "application/json", "{\"error\":\"Push status memory unavailable\",\"retryable\":true}");
            return;
        }
        const size_t expected = measureJson(doc);
        if (expected == 0 || expected > 16u * 1024u) {
            server.send(503, "application/json", "{\"error\":\"Push status unavailable\",\"retryable\":true}");
            return;
        }
        WifiApiResponse::sendJsonDocument(server, 200, doc);
        return;
    }
    String json;
    if (runtime.loadPushStatusJson && runtime.loadPushStatusJson(json, runtime.loadPushStatusJsonCtx)) {
        server.send(200, "application/json", json);
        return;
    }
    server.send(500, "application/json", "{\"error\":\"Push status not available\"}");
}

template <typename Form>
void handleApiSlotSaveImpl(WebServer& server, const Runtime& runtime, const Form& form,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;

    SlotsSnapshot current;
    if (runtime.loadSlotsSnapshot) runtime.loadSlotsSnapshot(current, runtime.loadSlotsSnapshotCtx);
    const bool profileOwned = current.profileOwned;
    if (!form.has("slot") || !form.has("profile") || (!profileOwned && !form.has("mode"))) {
        server.send(400, "application/json", "{\"error\":\"Missing parameters\"}");
        return;
    }

    if (profileOwned &&
        (form.has("mode") || form.has("volumeConfigured") || form.has("volume") ||
         form.has("muteVol") || form.has("muteVolume") || form.has("mainVolume") ||
         form.has("mutedVolume") || form.has("darkMode") || form.has("muteToZero"))) {
        server.send(400, "application/json",
                    "{\"error\":\"Detector settings belong to the selected profile\"}");
        return;
    }

    int slot = -1;
    String profile;
    bool clearProfile = false;
    if (!parseIntArg(form, "slot", slot, true) ||
        !readPresentArg(form, "profile", profile, true) ||
        !parseBoolArg(form, "clearProfile", clearProfile)) {
        server.send(400, "application/json", "{\"error\":\"Invalid parameters\"}");
        return;
    }
    // Empty is a real operation (unassign), so require an explicit companion
    // token. If WebServer::arg() cannot allocate a non-empty profile value, it
    // becomes empty; without this token that failure must not be mistaken for
    // a successful unassignment.
    if (profile.length() == 0 && !clearProfile) {
        server.send(503, "application/json", "{\"error\":\"Profile parameter unavailable\",\"retryable\":true}");
        return;
    }
    if (profile.length() > 0 && clearProfile) {
        server.send(400, "application/json", "{\"error\":\"Conflicting profile assignment\"}");
        return;
    }
    int mode = 0;
    String name;
    const bool hasName = form.has("name");
    if ((!profileOwned && !parseIntArg(form, "mode", mode, true)) ||
        (hasName && !readPresentArg(form, "name", name))) {
        server.send(400, "application/json", "{\"error\":\"Invalid slot name\"}");
        return;
    }
    if ((hasName && !isCanonicalSlotNameValue(name)) ||
        (!profileOwned && (mode < V1_MODE_UNKNOWN || mode > V1_MODE_ADVANCED_LOGIC))) {
        server.send(400, "application/json", "{\"error\":\"Non-canonical slot settings\"}");
        return;
    }
    int color = -1;
    int volume = -1;
    int muteVol = -1;
    int alertPersist = -1;
    if (!parseIntArg(form, "color", color) || !parseIntArg(form, "volume", volume) ||
        !parseIntArg(form, "muteVol", muteVol) || !parseIntArg(form, "alertPersist", alertPersist)) {
        server.send(400, "application/json", "{\"error\":\"Invalid numeric parameter\"}");
        return;
    }
    const bool hasVolumeConfigured = form.has("volumeConfigured");
    bool volumeConfigured = false;
    if (!parseBoolArg(form, "volumeConfigured", volumeConfigured)) {
        server.send(400, "application/json", "{\"error\":\"Invalid volume policy\"}");
        return;
    }
    bool hasDarkMode = form.has("darkMode");
    bool darkMode = false;
    if (!parseBoolArg(form, "darkMode", darkMode)) {
        server.send(400, "application/json", "{\"error\":\"Invalid dark-mode policy\"}");
        return;
    }
    bool hasMuteToZero = form.has("muteToZero");
    bool muteToZero = false;
    if (!parseBoolArg(form, "muteToZero", muteToZero)) {
        server.send(400, "application/json", "{\"error\":\"Invalid mute policy\"}");
        return;
    }
    bool hasAlertPersist = form.has("alertPersist");
    bool priorityArrowOnly = false;
    if (!parseBoolArg(form, "priorityArrowOnly", priorityArrowOnly)) {
        server.send(400, "application/json", "{\"error\":\"Invalid arrow policy\"}");
        return;
    }

    if (slot < 0 || slot > 2) {
        server.send(400, "application/json", "{\"error\":\"Invalid slot\"}");
        return;
    }
    if ((form.has("color") && (color < 1 || color > 0xFFFF)) ||
        (hasAlertPersist && (alertPersist < 0 || alertPersist > 5))) {
        server.send(400, "application/json", "{\"error\":\"Slot value out of range\"}");
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
        profile = std::move(canonicalProfile);
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
        if (volumeConfigured && (!form.has("volume") || !form.has("muteVol") || volume < 0 || volume > 9 ||
                                 muteVol < 0 || muteVol > 9)) {
            server.send(400, "application/json", "{\"error\":\"Both main and mute volume must be between 0 and 9\"}");
            return;
        }
        if (!volumeConfigured && (form.has("volume") || form.has("muteVol"))) {
            server.send(400, "application/json", "{\"error\":\"Disabled volume policy cannot carry values\"}");
            return;
        }
    } else if (form.has("volume") != form.has("muteVol") ||
               ((form.has("volume") || form.has("muteVol")) &&
                (volume < 0 || volume > 9 || muteVol < 0 || muteVol > 9))) {
        server.send(400, "application/json", "{\"error\":\"Main and mute volume must be configured together\"}");
        return;
    }

    bool persisted = false;

    if (runtime.applySlotUpdate) {
        SlotUpdateRequest request;
        request.slot = slot;
        request.profileOwned = profileOwned;
        request.hasName = hasName;
        request.name = std::move(name);
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
        request.hasPriorityArrowOnly = form.has("priorityArrowOnly");
        request.priorityArrowOnly = priorityArrowOnly;
        request.profile = std::move(profile);
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

        if (form.has("priorityArrowOnly") && runtime.setSlotPriorityArrowOnly) {
            runtime.setSlotPriorityArrowOnly(slot, priorityArrowOnly, runtime.setSlotPriorityArrowOnlyCtx);
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

template <typename Form>
void handleApiActivateImpl(WebServer& server, const Runtime& runtime, const Form& form,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;

    if (!form.has("slot")) {
        server.send(400, "application/json", "{\"error\":\"Missing slot parameter\"}");
        return;
    }

    int slot = -1;
    bool enable = true;
    if (!parseIntArg(form, "slot", slot, true) || !parseBoolArg(form, "enable", enable)) {
        server.send(400, "application/json", "{\"error\":\"Invalid activation parameters\"}");
        return;
    }

    if (slot < 0 || slot > 2) {
        server.send(400, "application/json", "{\"error\":\"Invalid slot\"}");
        return;
    }

    if (runtime.applyActivation) {
        ActivationRequest request;
        request.slot = slot;
        request.enable = enable;
        if (!runtime.applyActivation(request, runtime.applyActivationCtx)) {
            server.send(500, "application/json", "{\"success\":false,\"error\":\"settings_persist_failed\"}");
            return;
        }
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

void handleApiSlotSave(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                       void* rateLimitCtx) {
    const WebServerForm form{server};
    handleApiSlotSaveImpl(server, runtime, form, checkRateLimit, rateLimitCtx);
}

void handleApiSlotSaveBody(WebServer& server, const Runtime& runtime, const uint8_t* body, size_t bodySize,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                           const char* multipartBoundary, size_t multipartBoundarySize) {
    const ExactUrlEncodedForm parsed = multipartBoundarySize == 0
        ? ExactUrlEncodedForm(body, bodySize)
        : ExactUrlEncodedForm(body, bodySize, multipartBoundary, multipartBoundarySize);
    static constexpr const char* ALLOWED[] = {
        "slot", "profile", "clearProfile", "mode", "name", "color", "volumeConfigured",
        "volume", "muteVol", "darkMode", "muteToZero", "alertPersist", "priorityArrowOnly",
    };
    if (!parsed.valid() || !parsed.hasOnly(ALLOWED, sizeof(ALLOWED) / sizeof(ALLOWED[0]))) {
        server.send(400, "application/json", "{\"error\":\"Invalid form body\"}");
        return;
    }
    const ExactBodyForm form{parsed};
    handleApiSlotSaveImpl(server, runtime, form, checkRateLimit, rateLimitCtx);
}

void handleApiActivate(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                       void* rateLimitCtx) {
    const WebServerForm form{server};
    handleApiActivateImpl(server, runtime, form, checkRateLimit, rateLimitCtx);
}

void handleApiActivateBody(WebServer& server, const Runtime& runtime, const uint8_t* body, size_t bodySize,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                           const char* multipartBoundary, size_t multipartBoundarySize) {
    const ExactUrlEncodedForm parsed = multipartBoundarySize == 0
        ? ExactUrlEncodedForm(body, bodySize)
        : ExactUrlEncodedForm(body, bodySize, multipartBoundary, multipartBoundarySize);
    static constexpr const char* ALLOWED[] = {"slot", "enable"};
    if (!parsed.valid() || !parsed.hasOnly(ALLOWED, sizeof(ALLOWED) / sizeof(ALLOWED[0]))) {
        server.send(400, "application/json", "{\"error\":\"Invalid form body\"}");
        return;
    }
    const ExactBodyForm form{parsed};
    handleApiActivateImpl(server, runtime, form, checkRateLimit, rateLimitCtx);
}

} // namespace WifiAutoPushApiService
