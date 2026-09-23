// The NVS intent remains authoritative until the device row and fallback
// converge, so interruption can delay deletion but cannot resurrect the row.

#include "settings_internals.h"
#include "settings_sanitize.h"
#include "v1_devices.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr uint8_t INTENT_VERSION = 1;
constexpr uint8_t PHASE_STAGED = 1;
constexpr uint8_t PHASE_FALLBACK_CLEARED = 2;
constexpr uint8_t PHASE_PRIMARY_COMMITTED = 3;
constexpr uint8_t PHASE_MIRRORED = 4;

enum class IntentReadStatus : uint8_t { Absent = 0, Valid, Invalid, Unavailable };

struct DeleteIntent {
    String address;
    uint32_t token = 0;
    uint8_t phase = 0;
};

bool canonicalAddressBytes(const String& value) {
    if (value.length() != 17u) return false;
    for (size_t index = 0; index < 17u; ++index) {
        const char byte = value[index];
        if ((index + 1u) % 3u == 0u) {
            if (byte != ':') return false;
        } else if (!((byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'F'))) {
            return false;
        }
    }
    return true;
}

bool readExactAddress(Preferences& prefs, String& output) {
    if (prefs.getType(kNvsV1DeleteAddress) != PT_STR || prefs.getStringLength(kNvsV1DeleteAddress) != 18u) {
        return false;
    }
    String value = prefs.getString(kNvsV1DeleteAddress, "");
    if (value.length() != 17u || std::strlen(value.c_str()) != 17u || !canonicalAddressBytes(value)) return false;
    output = std::move(value);
    return output.length() == 17u;
}

IntentReadStatus readIntent(DeleteIntent& intent) {
    intent = DeleteIntent{};
    Preferences prefs;
    if (!prefs.begin(kSettingsV1RuntimeNamespace, true)) return IntentReadStatus::Unavailable;
    if (!prefs.isKey(kNvsV1DeleteReady)) {
        prefs.end();
        return IntentReadStatus::Absent;
    }
    if (prefs.getType(kNvsV1DeleteReady) != PT_U8 || prefs.getUChar(kNvsV1DeleteReady, 0) != INTENT_VERSION ||
        prefs.getType(kNvsV1DeleteToken) != PT_U32 || prefs.getType(kNvsV1DeletePhase) != PT_U8 ||
        !readExactAddress(prefs, intent.address)) {
        prefs.end();
        return IntentReadStatus::Invalid;
    }
    intent.token = prefs.getUInt(kNvsV1DeleteToken, 0);
    intent.phase = prefs.getUChar(kNvsV1DeletePhase, 0);
    prefs.end();
    if (intent.token == 0 || intent.phase < PHASE_STAGED || intent.phase > PHASE_MIRRORED) {
        return IntentReadStatus::Invalid;
    }
    return IntentReadStatus::Valid;
}

bool removeIntentKeys(Preferences& prefs, bool includeReady) {
    static constexpr const char* kPayloadKeys[] = {
        kNvsV1DeleteAddress, kNvsV1DeleteToken, kNvsV1DeletePhase,
    };
    if (includeReady && prefs.isKey(kNvsV1DeleteReady) && !prefs.remove(kNvsV1DeleteReady)) return false;
    for (const char* key : kPayloadKeys) {
        if (prefs.isKey(key) && !prefs.remove(key)) return false;
    }
    return true;
}

bool stageIntent(const String& address, DeleteIntent& intent) {
    Preferences prefs;
    if (!prefs.begin(kSettingsV1RuntimeNamespace, false)) return false;
    if (!removeIntentKeys(prefs, true)) {
        prefs.end();
        return false;
    }
    const uint32_t token = std::max<uint32_t>(1u, static_cast<uint32_t>(micros()));
    const bool payloadWritten =
        prefs.putString(kNvsV1DeleteAddress, address) == address.length() &&
        prefs.getStringLength(kNvsV1DeleteAddress) == address.length() + 1u &&
        prefs.putUInt(kNvsV1DeleteToken, token) == sizeof(token) &&
        prefs.getUInt(kNvsV1DeleteToken, 0) == token &&
        prefs.putUChar(kNvsV1DeletePhase, PHASE_STAGED) == sizeof(uint8_t) &&
        prefs.getUChar(kNvsV1DeletePhase, 0) == PHASE_STAGED;
    // Never call the ready write after a payload failure. A partial payload is
    // non-authoritative and remains retryable because deletion has not begun.
    const bool readyWritten = payloadWritten &&
        prefs.putUChar(kNvsV1DeleteReady, INTENT_VERSION) == sizeof(uint8_t) &&
        prefs.getUChar(kNvsV1DeleteReady, 0) == INTENT_VERSION;
    prefs.end();
    return readyWritten && readIntent(intent) == IntentReadStatus::Valid &&
           intent.address == address && intent.token == token && intent.phase == PHASE_STAGED;
}

bool advanceIntent(const DeleteIntent& expected, uint8_t phase, DeleteIntent& updated) {
    if (phase <= expected.phase || phase > PHASE_MIRRORED) return false;
    Preferences prefs;
    if (!prefs.begin(kSettingsV1RuntimeNamespace, false)) return false;
    const bool written = prefs.putUChar(kNvsV1DeletePhase, phase) == sizeof(uint8_t) &&
                         prefs.getUChar(kNvsV1DeletePhase, 0) == phase;
    prefs.end();
    if (!written || readIntent(updated) != IntentReadStatus::Valid) return false;
    return updated.address == expected.address && updated.token == expected.token && updated.phase == phase;
}

bool clearIntent(const DeleteIntent& expected) {
    DeleteIntent current;
    if (readIntent(current) != IntentReadStatus::Valid || current.address != expected.address ||
        current.token != expected.token) return false;
    Preferences prefs;
    if (!prefs.begin(kSettingsV1RuntimeNamespace, false)) return false;
    // Remove authority first. Any interrupted payload cleanup after this point
    // is harmless because deletion effects were already verified.
    const bool removed = removeIntentKeys(prefs, true);
    prefs.end();
    DeleteIntent ignored;
    return removed && readIntent(ignored) == IntentReadStatus::Absent;
}

} // namespace

V1DeviceMutationResult SettingsManager::deleteV1DeviceTransactional(const String& address,
                                                                      V1DeviceStore& devices) {
    if (!canonicalAddressBytes(address)) return {V1DeviceMutationStatus::Invalid};
    if (!devices.catalogReadable() || !devices.flushPendingSave()) {
        return {V1DeviceMutationStatus::Unavailable};
    }

    DeleteIntent intent;
    IntentReadStatus intentStatus = readIntent(intent);
    if (intentStatus == IntentReadStatus::Invalid || intentStatus == IntentReadStatus::Unavailable) {
        return {V1DeviceMutationStatus::Unavailable};
    }
    if (intentStatus == IntentReadStatus::Valid && intent.address != address) {
        const V1DeviceMutationResult prior = deleteV1DeviceTransactional(intent.address, devices);
        if (!prior.fullyMirrored()) return {V1DeviceMutationStatus::Busy};
        intentStatus = IntentReadStatus::Absent;
    }
    if (intentStatus == IntentReadStatus::Absent && !stageIntent(address, intent)) {
        return {V1DeviceMutationStatus::NotCommitted};
    }

    if (intent.phase == PHASE_STAGED) {
        if (!clearLastV1AddressFallback(intent.address)) {
            return {V1DeviceMutationStatus::DurablePending};
        }
        DeleteIntent advanced;
        if (!advanceIntent(intent, PHASE_FALLBACK_CLEARED, advanced)) {
            return {V1DeviceMutationStatus::DurablePending};
        }
        intent = std::move(advanced);
    }

    if (intent.phase == PHASE_FALLBACK_CLEARED || intent.phase == PHASE_PRIMARY_COMMITTED) {
        const V1DeviceMutationResult removed = devices.removeDevice(intent.address);
        if (!removed.committed()) return {V1DeviceMutationStatus::DurablePending};
        const uint8_t nextPhase = removed.fullyMirrored() ? PHASE_MIRRORED : PHASE_PRIMARY_COMMITTED;
        if (intent.phase != nextPhase) {
            DeleteIntent advanced;
            if (!advanceIntent(intent, nextPhase, advanced)) {
                return {V1DeviceMutationStatus::DurablePending};
            }
            intent = std::move(advanced);
        }
        if (!removed.fullyMirrored()) return {V1DeviceMutationStatus::DurablePending};
    }

    if (!clearLastV1AddressFallback(intent.address)) {
        return {V1DeviceMutationStatus::DurablePending};
    }
    const V1DeviceMutationResult converged = devices.removeDevice(intent.address);
    bool stillPresent = true;
    if (!converged.fullyMirrored() || !devices.containsDeviceChecked(intent.address, stillPresent) || stillPresent ||
        !clearIntent(intent)) {
        return {V1DeviceMutationStatus::DurablePending};
    }
    return {V1DeviceMutationStatus::FullyMirrored};
}

bool SettingsManager::resolvePendingV1DeviceDelete(V1DeviceStore& devices) {
    DeleteIntent intent;
    const IntentReadStatus status = readIntent(intent);
    if (status == IntentReadStatus::Absent) return true;
    if (status != IntentReadStatus::Valid) return false;
    return deleteV1DeviceTransactional(intent.address, devices).fullyMirrored();
}
