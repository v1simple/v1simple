/**
 * Shared internals for settings translation units.
 *
 * Include this instead of settings.h in the settings_*.cpp files
 * so that cross-TU constants, crypto helpers, and backup utilities
 * are available without duplicating declarations.
 */

#pragma once

#include "settings.h"
#include "settings_namespace_ids.h"
#include "settings_keys.h"
#include "settings_sanitize.h"
#include "storage_manager.h"
#include "v1_profiles.h"
#include <ArduinoJson.h>
#include <algorithm>

// ── Shared NVS / SD constants ──────────────────────────────────────────────

extern const char* SETTINGS_BACKUP_PATH;
extern const char* SETTINGS_BACKUP_TMP_PATH;
extern const char* SETTINGS_BACKUP_PREV_PATH;
inline constexpr int SD_BACKUP_VERSION = 18;
extern const size_t SETTINGS_BACKUP_MAX_BYTES;
extern const char* WIFI_CLIENT_NS;
extern const char* WIFI_CLIENT_SD_SECRET_PATH;
extern const char* WIFI_CLIENT_SD_SECRET_TYPE;
extern const int   WIFI_CLIENT_SD_SECRET_VERSION;
extern const char* const SETTINGS_BACKUP_CANDIDATES[];
extern const size_t      SETTINGS_BACKUP_CANDIDATES_COUNT;

extern const char  XOR_KEY[];
inline constexpr int SETTINGS_VERSION = 10;
extern const char* OBFUSCATION_HEX_PREFIX;

// ── Static helpers promoted to internal-linkage-free functions ──────────────

WiFiModeSetting clampWifiModeValue(int raw);
VoiceAlertMode  clampVoiceAlertModeValue(int raw);
String sanitizeApPasswordValue(const String& raw);
String sanitizeLastV1AddressValue(const String& raw);
String sanitizeObdSavedNameValue(const String& raw);

// Backup file helpers
bool isSupportedBackupType(const JsonDocument& doc);
bool hasBackupSignature(const JsonDocument& doc);
bool parseBackupFile(fs::FS* fs, const char* path, JsonDocument& doc, bool verboseErrors = true);
int  backupDocumentVersion(const JsonDocument& doc);
int  backupCriticalFieldScore(const JsonDocument& doc);
int  backupCandidateScore(const JsonDocument& doc);
bool loadBestBackupDocument(fs::FS* fs, JsonDocument& outDoc,
                            const char** outPath = nullptr, bool verboseErrors = false);
bool parseBoolVariant(const JsonVariantConst& value, bool& out);

struct SerializedSettingsBackupPayload {
    char* data = nullptr;
    size_t capacity = 0;
    size_t length = 0;
    bool inPsram = false;
    bool protectExistingBackupFromProvisionalNvs = false;
    uint32_t snapshotMs = 0;
    int profilesBackedUp = 0;
};

bool buildSerializedSdBackupPayload(SerializedSettingsBackupPayload& payload,
                                    const V1Settings& settings,
                                    const V1ProfileManager& profileManager,
                                    uint32_t snapshotMs);
void releaseSerializedSettingsBackupPayload(SerializedSettingsBackupPayload& payload);
bool writeBackupAtomically(fs::FS* fs, const SerializedSettingsBackupPayload& payload);

// Signals the deferred-backup writer task to drain its queue and exit, and
// blocks (up to timeoutMs) for the task to terminate. Subsequent calls to
// SettingsManager backup APIs become no-ops because the writer task will not
// be respawned after shutdown is requested. Intended for the car-power-off
// teardown sequence; tests reset the shutdown flag via
// resetDeferredSettingsBackupStateForTest().
void shutdownDeferredSettingsBackupWriter(uint32_t timeoutMs);

#ifdef UNIT_TEST
void resetDeferredSettingsBackupStateForTest();
bool runDeferredSettingsBackupWriterOnceForTest();
size_t deferredSettingsBackupQueueDepthForTest();
bool deferredSettingsBackupPendingForTest();
#endif

// NVS helpers
bool   attemptNvsRecovery(const char* activeNs);
int    namespaceHealthScore(const char* ns);
bool   isKnownSettingsNamespace(const String& ns);

struct SettingsNamespaceCleanupPlan {
    bool shouldCleanup = false;
    const char* inactiveNamespace = nullptr;
    bool clearLegacyNamespace = false;
};

inline SettingsNamespaceCleanupPlan buildSettingsNamespaceCleanupPlan(uint32_t usedPct,
                                                                     const String& activeNs,
                                                                     bool hasSdBackup) {
    SettingsNamespaceCleanupPlan plan;
    if (usedPct <= 80) {
        return plan;
    }

    if (activeNs == SETTINGS_NS_A) {
        plan.shouldCleanup = true;
        plan.inactiveNamespace = SETTINGS_NS_B;
        plan.clearLegacyNamespace = hasSdBackup;
        return plan;
    }
    if (activeNs == SETTINGS_NS_B) {
        plan.shouldCleanup = true;
        plan.inactiveNamespace = SETTINGS_NS_A;
        plan.clearLegacyNamespace = hasSdBackup;
        return plan;
    }

    // If the active namespace is legacy or unknown, avoid destructive cleanup.
    return plan;
}

// Crypto / obfuscation
String xorObfuscate(const String& input);
char   hexDigit(uint8_t nibble);
int    hexNibble(char c);
String bytesToHex(const String& input);
bool   hexToBytes(const String& input, String& out);
String encodeObfuscatedForStorage(const String& plainText);
String decodeObfuscatedFromStorage(const String& stored);

// CRC32 (IEEE 802.3 polynomial 0xEDB88320).
// Standard check value: computeCrc32("123456789", 9) == 0xCBF43926.
uint32_t computeCrc32(const uint8_t* data, size_t length);

// WiFi client SD secret helpers
bool   saveWifiClientSecretToSD(size_t slotIndex, const String& ssid, const String& encodedPassword);
String loadWifiClientSecretFromSD(const String& expectedSsid, size_t expectedSlotIndex = kWifiStaSlotCount);
void   removeWifiClientSecretFromSD(size_t slotIndex, const String& ssid);
void   clearWifiClientSecretFromSD();
bool   storeWifiClientPasswordObfToNvs(const String& encodedPassword, size_t slotIndex = 0);
