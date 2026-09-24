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
#include "v1_devices.h"
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

template <typename Form>
bool parseUint32Arg(const Form& form, const char* key, uint32_t& value, bool required = false) {
    if (!form.has(key)) return !required;
    String raw;
    if (!readPresentArg(form, key, raw)) return false;
    if (raw.length() == 0 || (raw.length() > 1 && raw[0] == '0')) return false;
    uint32_t parsed = 0;
    for (size_t index = 0; index < raw.length(); ++index) {
        const char byte = raw[index];
        if (byte < '0' || byte > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(byte - '0');
        if (parsed > (UINT32_MAX - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    if (parsed == 0) return false;
    value = parsed;
    return true;
}

bool validateTarget(WebServer& server, const Runtime& runtime, const String& address,
                    bool requireGen2) {
    if (!runtime.validateOperationTarget) {
        server.send(503, "application/json",
                    "{\"success\":false,\"error\":\"target_validation_unavailable\",\"retryable\":true}");
        return false;
    }
    switch (runtime.validateOperationTarget(address, requireGen2,
                                            runtime.validateOperationTargetCtx)) {
    case OperationTargetStatus::Allowed:
        return true;
    case OperationTargetStatus::NotFound:
        server.send(404, "application/json",
                    "{\"success\":false,\"error\":\"captured_target_not_found\"}");
        return false;
    case OperationTargetStatus::UnsupportedFirmware:
        server.send(409, "application/json",
                    "{\"success\":false,\"error\":\"factory_reset_requires_captured_gen2\"}");
        return false;
    case OperationTargetStatus::Unavailable:
        server.send(503, "application/json",
                    "{\"success\":false,\"error\":\"target_validation_unavailable\",\"retryable\":true}");
        return false;
    }
    return false;
}

bool canonicalAddress(const ExactUrlEncodedForm& form, String& address) {
    if (!form.read("address", address) || address.length() != 17u) return false;
    return V1SettingsOperationStore::isCanonicalAddress(address.c_str());
}

void sendOperationStartFailure(WebServer& server, V1SettingsOperationStore::StartStatus status,
                               uint32_t activeOperationId) {
    switch (status) {
    case V1SettingsOperationStore::StartStatus::Active: {
        char body[128];
        std::snprintf(body, sizeof(body),
                      "{\"success\":false,\"error\":\"operation_active\",\"operationId\":%lu}",
                      static_cast<unsigned long>(activeOperationId));
        server.send(409, "application/json", body);
        return;
    }
    case V1SettingsOperationStore::StartStatus::Invalid:
        server.send(400, "application/json", "{\"success\":false,\"error\":\"invalid_operation\"}");
        return;
    case V1SettingsOperationStore::StartStatus::IdentityExhausted:
        server.send(507, "application/json", "{\"success\":false,\"error\":\"operation_identity_exhausted\"}");
        return;
    case V1SettingsOperationStore::StartStatus::StorageUnavailable:
        server.send(503, "application/json",
                    "{\"success\":false,\"error\":\"operation_storage_unavailable\",\"retryable\":true}");
        return;
    case V1SettingsOperationStore::StartStatus::Started:
        break;
    }
    server.send(500, "application/json", "{\"success\":false,\"error\":\"operation_start_failed\"}");
}

void sendStartedOperation(WebServer& server, const Runtime& runtime,
                          const OperationStartRequest& request) {
    if (!runtime.startOperation || !runtime.restartForOperation) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"operation_runtime_unavailable\"}");
        return;
    }
    const V1SettingsOperationStore::StartResult started =
        runtime.startOperation(request, runtime.startOperationCtx);
    if (started.status != V1SettingsOperationStore::StartStatus::Started) {
        sendOperationStartFailure(server, started.status, started.operationId);
        return;
    }
    char body[192];
    std::snprintf(body, sizeof(body),
                  "{\"success\":true,\"queued\":true,\"operationId\":%lu,"
                  "\"state\":\"pending_normal_boot\",\"rebooting\":true,\"target\":\"normal\"}",
                  static_cast<unsigned long>(started.operationId));
    server.send(202, "application/json", body);
    runtime.restartForOperation(runtime.restartForOperationCtx);
}

void appendDurableOperation(JsonObject root, const V1SettingsOperationStore::Snapshot& snapshot) {
    root["operationId"] = snapshot.operationId;
    root["kind"] = V1SettingsOperationStore::kindName(snapshot.kind);
    root["source"] = V1SettingsOperationStore::sourceName(snapshot.source);
    root["state"] = V1SettingsOperationStore::stateName(snapshot.state);
    root["reason"] = V1SettingsOperationStore::reasonName(snapshot.reason);
    root["terminal"] = snapshot.state == V1SettingsOperationStore::State::Succeeded ||
                       snapshot.state == V1SettingsOperationStore::State::Partial ||
                       snapshot.state == V1SettingsOperationStore::State::Failed;
    root["result"] = snapshot.state == V1SettingsOperationStore::State::Succeeded
                         ? "succeeded"
                         : (snapshot.state == V1SettingsOperationStore::State::Partial
                                ? "partial"
                                : (snapshot.state == V1SettingsOperationStore::State::Failed
                                       ? "failed" : "in_progress"));
    if (snapshot.slot >= 0) root["slot"] = snapshot.slot;
    else root["slot"] = nullptr;
    if (snapshot.profileName[0] != '\0') root["profileName"] = snapshot.profileName;
    else root["profileName"] = nullptr;
    root["targetAddress"] = snapshot.targetAddress;
    root["returnToMaintenance"] = snapshot.returnToMaintenance;
    if (snapshot.executorOperationId != 0) root["executorOperationId"] = snapshot.executorOperationId;
    else root["executorOperationId"] = nullptr;
    JsonObject components = root["components"].to<JsonObject>();
    for (size_t index = 0; index < V1SettingsOperationStore::kComponentCount; ++index) {
        const auto component = static_cast<V1SettingsOperationStore::Component>(index);
        const auto& saved = snapshot.components[index];
        JsonObject item = components[V1SettingsOperationStore::componentName(component)].to<JsonObject>();
        item["requested"] = saved.requested;
        item["sent"] = saved.sent;
        item["verified"] = saved.verified;
        item["applied"] = saved.verified;
        item["outcome"] = V1SettingsOperationStore::componentOutcomeName(saved.outcome);
        item["reason"] = V1SettingsOperationStore::componentReasonName(saved.reason);
    }
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
        if (snapshot.profileOwned) {
            obj["volumeConfigured"] = slot.volumeConfigured;
            obj["volume"] = slot.volumeConfigured ? slot.volume : 0;
            obj["muteVolume"] = slot.volumeConfigured ? slot.muteVolume : 0;
            obj["darkModeConfigured"] = slot.darkModeConfigured;
            obj["darkMode"] = slot.darkMode;
        }
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
    handleApiStatusQuery(server, runtime, nullptr, 0);
}

void handleApiStatusQuery(WebServer& server, const Runtime& runtime,
                          const uint8_t* query, size_t querySize) {
    uint32_t requestedOperationId = 0;
    if (querySize > 0) {
        const ExactUrlEncodedForm form(query, querySize);
        static constexpr const char* ALLOWED[] = {"operationId"};
        if (!form.valid() || !form.hasOnly(ALLOWED, 1) ||
            !parseUint32Arg(form, "operationId", requestedOperationId, true)) {
            server.send(400, "application/json", "{\"error\":\"invalid_operation_id\"}");
            return;
        }
    }
    if (runtime.loadOperation) {
        V1SettingsOperationStore::Snapshot snapshot;
        if (!runtime.loadOperation(snapshot, runtime.loadOperationCtx)) {
            server.send(503, "application/json",
                        "{\"error\":\"operation_status_unavailable\",\"retryable\":true}");
            return;
        }
        if (!snapshot.available) {
            server.send(404, "application/json", "{\"error\":\"operation_not_found\"}");
            return;
        }
        if (requestedOperationId != 0 && requestedOperationId != snapshot.operationId) {
            char body[160];
            std::snprintf(body, sizeof(body),
                          "{\"error\":\"operation_mismatch\",\"requestedOperationId\":%lu,"
                          "\"currentOperationId\":%lu}",
                          static_cast<unsigned long>(requestedOperationId),
                          static_cast<unsigned long>(snapshot.operationId));
            server.send(409, "application/json", body);
            return;
        }
        PsramJson::Document doc;
        appendDurableOperation(doc.to<JsonObject>(), snapshot);
        if (doc.overflowed() || measureJson(doc) == 0) {
            server.send(503, "application/json", "{\"error\":\"operation_status_memory_unavailable\"}");
            return;
        }
        WifiApiResponse::sendJsonDocument(server, 200, doc);
        return;
    }
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

void handleApiApplySlotBody(WebServer& server, const Runtime& runtime,
                            const uint8_t* body, size_t bodySize) {
    const ExactUrlEncodedForm form(body, bodySize);
    static constexpr const char* ALLOWED[] = {"slot", "address"};
    int slot = -1;
    String address;
    if (!form.valid() || !form.hasOnly(ALLOWED, 2) || !parseIntArg(form, "slot", slot, true) ||
        slot < 0 || slot > 2 || !canonicalAddress(form, address)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"invalid_apply_request\"}");
        return;
    }
    SlotsSnapshot slots;
    if (!runtime.loadSlotsSnapshotResult ||
        !runtime.loadSlotsSnapshotResult(slots, runtime.loadSlotsSnapshotResultCtx)) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"slot_unavailable\"}");
        return;
    }
    if (!slots.profileOwned || slots.slots[slot].profile.length() == 0) {
        server.send(409, "application/json", "{\"success\":false,\"error\":\"slot_profile_required\"}");
        return;
    }
    if (!validateTarget(server, runtime, address, false)) return;
    OperationStartRequest request;
    request.kind = V1SettingsOperationStore::Kind::ApplySlot;
    request.slot = slot;
    request.targetAddress = std::move(address);
    sendStartedOperation(server, runtime, request);
}

void handleApiApplyProfileBody(WebServer& server, const Runtime& runtime,
                               const uint8_t* body, size_t bodySize) {
    const ExactUrlEncodedForm form(body, bodySize);
    static constexpr const char* ALLOWED[] = {"profile", "address"};
    String profile;
    String address;
    if (!form.valid() || !form.hasOnly(ALLOWED, 2) || !form.read("profile", profile) ||
        !canonicalAddress(form, address)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"invalid_apply_request\"}");
        return;
    }
    String canonical;
    if (canonicalizeProfileName(profile, canonical) != ProfileNameStatus::Valid || canonical != profile) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"invalid_profile_name\"}");
        return;
    }
    if (!runtime.validateProfileAssignment) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"profile_validation_unavailable\"}");
        return;
    }
    switch (runtime.validateProfileAssignment(canonical, runtime.validateProfileAssignmentCtx)) {
    case ProfileAssignmentStatus::Success: break;
    case ProfileAssignmentStatus::NotFound:
        server.send(404, "application/json", "{\"success\":false,\"error\":\"profile_not_found\"}");
        return;
    case ProfileAssignmentStatus::InvalidName:
        server.send(400, "application/json", "{\"success\":false,\"error\":\"invalid_profile_name\"}");
        return;
    case ProfileAssignmentStatus::Busy:
        server.send(409, "application/json", "{\"success\":false,\"error\":\"profile_busy\",\"retryable\":true}");
        return;
    case ProfileAssignmentStatus::IoError:
    case ProfileAssignmentStatus::Corrupt:
        server.send(503, "application/json", "{\"success\":false,\"error\":\"profile_unavailable\",\"retryable\":true}");
        return;
    }
    if (!validateTarget(server, runtime, address, false)) return;
    OperationStartRequest request;
    request.kind = V1SettingsOperationStore::Kind::ApplyProfile;
    request.profileName = std::move(canonical);
    request.targetAddress = std::move(address);
    sendStartedOperation(server, runtime, request);
}

void handleApiFactoryResetBody(WebServer& server, const Runtime& runtime,
                               const uint8_t* body, size_t bodySize) {
    const ExactUrlEncodedForm form(body, bodySize);
    static constexpr const char* ALLOWED[] = {"address", "confirm"};
    String address;
    String confirmation;
    if (!form.valid() || !form.hasOnly(ALLOWED, 2) || !canonicalAddress(form, address) ||
        !form.read("confirm", confirmation) || confirmation != "RESET V1") {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"factory_reset_confirmation_required\"}");
        return;
    }
    if (!validateTarget(server, runtime, address, true)) return;
    OperationStartRequest request;
    request.kind = V1SettingsOperationStore::Kind::FactoryReset;
    request.targetAddress = std::move(address);
    sendStartedOperation(server, runtime, request);
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
        (form.has("mode") || form.has("muteVolume") || form.has("mainVolume") ||
         form.has("mutedVolume") || form.has("muteToZero"))) {
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
    const bool hasDarkModeConfigured = form.has("darkModeConfigured");
    bool darkModeConfigured = false;
    if (!parseBoolArg(form, "darkModeConfigured", darkModeConfigured) ||
        (profileOwned && ((hasDarkModeConfigured && darkModeConfigured && !hasDarkMode) ||
                          (hasDarkMode && (!hasDarkModeConfigured || !darkModeConfigured))))) {
        server.send(400, "application/json", "{\"error\":\"Invalid dark-mode override\"}");
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
    if (profileOwned && !hasVolumeConfigured && (form.has("volume") || form.has("muteVol"))) {
        server.send(400, "application/json", "{\"error\":\"Volume override choice required\"}");
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
        request.hasDarkMode = hasDarkMode || (profileOwned && hasDarkModeConfigured && !darkModeConfigured);
        request.darkMode = (!profileOwned || darkModeConfigured) ? darkMode : false;
        request.hasDarkModeConfigured = hasDarkModeConfigured;
        request.darkModeConfigured = darkModeConfigured;
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
        "volume", "muteVol", "darkMode", "darkModeConfigured", "muteToZero", "alertPersist", "priorityArrowOnly",
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
