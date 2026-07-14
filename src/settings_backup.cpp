/**
 * Settings SD backup writer and backup-file utilities.
 */

#include "settings_internals.h"
#include "backup_payload_builder.h"
#include "psram_freertos_alloc.h"

#include <atomic>
#include <esp_heap_caps.h>

// Obfuscation constants — declared extern in settings_internals.h
const char XOR_KEY[] = "V1G2-S3cr3t-K3y!";
const char* OBFUSCATION_HEX_PREFIX = "hex:";

// Obfuscate a string using XOR (same function for encode/decode)
String xorObfuscate(const String& input) {
    if (input.length() == 0)
        return input;

    String output;
    output.reserve(input.length());
    size_t keyLen = strlen(XOR_KEY);

    for (size_t i = 0; i < input.length(); i++) {
        output += (char)(input[i] ^ XOR_KEY[i % keyLen]);
    }
    return output;
}

char hexDigit(uint8_t nibble) {
    nibble &= 0x0F;
    return (nibble < 10) ? static_cast<char>('0' + nibble) : static_cast<char>('A' + (nibble - 10));
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

String bytesToHex(const String& input) {
    if (input.length() == 0)
        return "";
    String out;
    out.reserve(input.length() * 2);
    for (size_t i = 0; i < input.length(); ++i) {
        uint8_t b = static_cast<uint8_t>(input[i]);
        out += hexDigit(b >> 4);
        out += hexDigit(b);
    }
    return out;
}

bool hexToBytes(const String& input, String& out) {
    if ((input.length() % 2) != 0)
        return false;
    out = "";
    out.reserve(input.length() / 2);
    for (size_t i = 0; i < input.length(); i += 2) {
        int hi = hexNibble(input[i]);
        int lo = hexNibble(input[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        char decoded = static_cast<char>((hi << 4) | lo);
        out += decoded;
    }
    return true;
}

String encodeObfuscatedForStorage(const String& plainText) {
    if (plainText.length() == 0)
        return "";
    String obfuscated = xorObfuscate(plainText);
    String encoded = OBFUSCATION_HEX_PREFIX;
    encoded += bytesToHex(obfuscated);
    return encoded;
}

String decodeObfuscatedFromStorage(const String& stored) {
    if (stored.length() == 0)
        return "";

    if (stored.startsWith(OBFUSCATION_HEX_PREFIX)) {
        String hexPayload = stored.substring(strlen(OBFUSCATION_HEX_PREFIX));
        String obfuscated;
        if (!hexToBytes(hexPayload, obfuscated)) {
            Serial.println("[Settings] WARN: Invalid obfuscated hex payload");
            return "";
        }
        return xorObfuscate(obfuscated);
    }

    // Legacy format: raw XOR bytes stored directly as a String.
    return xorObfuscate(stored);
}

// CRC32 (IEEE 802.3, polynomial 0xEDB88320).
// Canonical table shared by BackupPayloadBuilder and V1ProfileManager.
// Verified against the standard check value: computeCrc32("123456789", 9) == 0xCBF43926.
static const uint32_t kCrc32Table[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3, 0x0EDB8832,
    0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
    0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7, 0x136C9856, 0x646BA8C0, 0xFD62F97A,
    0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
    0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3,
    0x45DF5C75, 0xDCD60DCF, 0xABD13D59, 0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
    0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB,
    0xB6662D3D, 0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01, 0x6B6B51F4,
    0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
    0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65, 0x4DB26158, 0x3AB551CE, 0xA3BC0074,
    0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
    0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9, 0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525,
    0x206F85B3, 0xB966D409, 0xCE61E49F, 0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
    0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615,
    0x73DC1683, 0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7, 0xFED41B76,
    0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
    0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B, 0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6,
    0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
    0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7,
    0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D, 0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
    0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7,
    0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45, 0xA00AE278,
    0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
    0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9, 0xBDBDF21C, 0xCABAC28A, 0x53B39330,
    0x24B4A3A6, 0xBAD03605, 0xCDD706B3, 0x54DE5729, 0x23D967BF, 0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
    0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D,
};

uint32_t computeCrc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc = kCrc32Table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// --- Backup file static helpers ---

namespace {

constexpr size_t SETTINGS_BACKUP_PAYLOAD_GROWTH_QUANTUM = 256u;

size_t roundUpSettingsBackupPayloadCapacity(size_t required) {
    return ((required + SETTINGS_BACKUP_PAYLOAD_GROWTH_QUANTUM - 1u) / SETTINGS_BACKUP_PAYLOAD_GROWTH_QUANTUM) *
           SETTINGS_BACKUP_PAYLOAD_GROWTH_QUANTUM;
}

bool writeSerializedBackupAtomically(fs::FS* fs, const char* data, size_t length) {
    if (!fs || !data || length == 0) {
        return false;
    }

    if (fs->exists(SETTINGS_BACKUP_TMP_PATH)) {
        fs->remove(SETTINGS_BACKUP_TMP_PATH);
    }

    File tmp = fs->open(SETTINGS_BACKUP_TMP_PATH, FILE_WRITE);
    if (!tmp) {
        Serial.println("[Settings] Failed to create temp SD backup file");
        return false;
    }

    const size_t written = tmp.write(reinterpret_cast<const uint8_t*>(data), length);
    tmp.flush();
    tmp.close();

    if (written != length) {
        Serial.println("[Settings] Failed to write temp SD backup");
        fs->remove(SETTINGS_BACKUP_TMP_PATH);
        return false;
    }

    JsonDocument verifyTmp;
    if (!parseBackupFile(fs, SETTINGS_BACKUP_TMP_PATH, verifyTmp, true)) {
        Serial.println("[Settings] Temp SD backup failed validation");
        fs->remove(SETTINGS_BACKUP_TMP_PATH);
        return false;
    }

    if (fs->exists(SETTINGS_BACKUP_PREV_PATH)) {
        fs->remove(SETTINGS_BACKUP_PREV_PATH);
    }

    bool rotatedPrimary = false;
    if (fs->exists(SETTINGS_BACKUP_PATH)) {
        if (fs->rename(SETTINGS_BACKUP_PATH, SETTINGS_BACKUP_PREV_PATH)) {
            rotatedPrimary = true;
        } else {
            Serial.println("[Settings] ERROR: Failed to rotate primary backup; keeping existing file");
            fs->remove(SETTINGS_BACKUP_TMP_PATH);
            return false;
        }
    }

    if (!fs->rename(SETTINGS_BACKUP_TMP_PATH, SETTINGS_BACKUP_PATH)) {
        Serial.println("[Settings] ERROR: Failed to promote temp backup to primary");
        fs->remove(SETTINGS_BACKUP_TMP_PATH);

        if (rotatedPrimary && fs->exists(SETTINGS_BACKUP_PREV_PATH) && !fs->exists(SETTINGS_BACKUP_PATH)) {
            if (!fs->rename(SETTINGS_BACKUP_PREV_PATH, SETTINGS_BACKUP_PATH)) {
                Serial.println("[Settings] CRITICAL: Failed to rollback previous backup");
            }
        }
        return false;
    }

    // Post-rename sanity: verify the promoted file exists with the expected
    // byte count.  A full JSON re-parse is unnecessary — the temp file was
    // already validated above and rename() is a metadata-only operation.
    File promoted = fs->open(SETTINGS_BACKUP_PATH, FILE_READ);
    if (!promoted || promoted.size() != length) {
        Serial.println("[Settings] ERROR: Promoted backup size mismatch after rename");
        if (promoted)
            promoted.close();
        fs->remove(SETTINGS_BACKUP_PATH);
        if (fs->exists(SETTINGS_BACKUP_PREV_PATH) && !fs->exists(SETTINGS_BACKUP_PATH)) {
            if (!fs->rename(SETTINGS_BACKUP_PREV_PATH, SETTINGS_BACKUP_PATH)) {
                Serial.println("[Settings] CRITICAL: Failed to restore previous backup after validation failure");
            }
        }
        return false;
    }
    promoted.close();

    return true;
}

} // namespace

bool isSupportedBackupType(const JsonDocument& doc) {
    if (!doc["_type"].is<const char*>()) {
        return true; // Legacy backups may not include a type marker.
    }
    return BackupPayloadBuilder::isRecognizedBackupType(doc["_type"].as<const char*>());
}

bool hasBackupSignature(const JsonDocument& doc) {
    // Require a small signature set to avoid accepting arbitrary JSON blobs.
    return doc["apSSID"].is<const char*>() || doc["brightness"].is<int>() || doc["colorBogey"].is<int>() ||
           doc["slot0Name"].is<const char*>();
}

bool parseBackupFile(fs::FS* fs, const char* path, JsonDocument& doc, bool verboseErrors) {
    if (!fs || !path || path[0] == '\0') {
        return false;
    }

    File file = fs->open(path, FILE_READ);
    if (!file) {
        if (verboseErrors) {
            Serial.printf("[Settings] Failed to open backup file: %s\n", path);
        }
        return false;
    }

    const size_t size = file.size();
    if (size == 0 || size > SETTINGS_BACKUP_MAX_BYTES) {
        if (verboseErrors) {
            Serial.printf("[Settings] Backup file size invalid (%u bytes): %s\n", static_cast<unsigned int>(size),
                          path);
        }
        file.close();
        return false;
    }

    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        if (verboseErrors) {
            Serial.printf("[Settings] Failed to parse backup '%s': %s\n", path, err.c_str());
        }
        return false;
    }

    if (!isSupportedBackupType(doc)) {
        if (verboseErrors) {
            Serial.printf("[Settings] Unsupported backup type in %s\n", path);
        }
        return false;
    }
    if (!hasBackupSignature(doc)) {
        if (verboseErrors) {
            Serial.printf("[Settings] Backup signature check failed for %s\n", path);
        }
        return false;
    }

    // CRC32 integrity check (only for backups that carry the field).
    // Older backups written before this feature was added will not have _crc32
    // and are accepted as-is.  A mismatch means media-level corruption; log
    // a warning but do NOT hard-reject — fall through to the partial-recovery
    // path rather than leaving the device with factory defaults.
    if (doc["_crc32"].is<uint32_t>()) {
        const uint32_t stored = doc["_crc32"].as<uint32_t>();
        const uint32_t computed = BackupPayloadBuilder::computeBackupCrc32(doc);
        if (stored != computed) {
            if (verboseErrors) {
                Serial.printf(
                    "[Settings] WARN: CRC32 mismatch in %s (stored=0x%08X computed=0x%08X) — backup may be corrupted\n",
                    path, stored, computed);
            }
            // Return false so loadBestBackupDocument skips this candidate and
            // falls through to the next (e.g. .prev).  If no valid candidate
            // exists the partial-recovery branch handles the rest.
            return false;
        }
    }

    return true;
}

int backupDocumentVersion(const JsonDocument& doc) {
    return doc["_version"] | doc["version"] | 1;
}

int backupCriticalFieldScore(const JsonDocument& doc) {
    int score = 0;
    if (!doc["brightness"].isNull())
        score++;
    if (!doc["proxyBLE"].isNull())
        score++;
    if (!doc["proxyName"].isNull())
        score++;
    if (!doc["wifiClientEnabled"].isNull())
        score++;
    if (!doc["wifiStaSlots"].isNull())
        score++;
    if (!doc["wifiClientPasswordObf"].isNull())
        score++;
    if (!doc["colorBogey"].isNull())
        score++;
    if (!doc["slot0ProfileName"].isNull())
        score++;
    if (!doc["slot1ProfileName"].isNull())
        score++;
    if (!doc["slot2ProfileName"].isNull())
        score++;
    return score;
}

int backupCandidateScore(const JsonDocument& doc) {
    // Prefer newer schema, then richer field coverage.
    return backupDocumentVersion(doc) * 100 + backupCriticalFieldScore(doc);
}

bool buildSerializedSdBackupPayload(SerializedSettingsBackupPayload& payload, const V1Settings& settings_,
                                    const V1ProfileManager& profileManager, uint32_t snapshotMs) {
    releaseSerializedSettingsBackupPayload(payload);

    JsonDocument doc;
    const BackupPayloadBuilder::BuildResult buildResult = BackupPayloadBuilder::buildBackupDocument(
        doc, settings_, profileManager, BackupPayloadBuilder::BackupTransport::SdBackup, snapshotMs);

    const size_t required = measureJson(doc) + 1u;
    const size_t capacity = roundUpSettingsBackupPayloadCapacity(required);
    char* data = static_cast<char*>(heap_caps_malloc(capacity, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    bool inPsram = true;

    if (data == nullptr) {
        data = static_cast<char*>(heap_caps_malloc(capacity, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        inPsram = false;
    }

    if (data == nullptr) {
        Serial.printf("[Settings] Failed to allocate serialized backup buffer (%lu bytes)\n",
                      static_cast<unsigned long>(capacity));
        return false;
    }

    const size_t length = serializeJson(doc, data, capacity);
    if (length == 0 || length >= capacity) {
        heap_caps_free(data);
        Serial.println("[Settings] Failed to serialize SD backup payload");
        return false;
    }
    data[length] = '\0';

    payload.data = data;
    payload.capacity = capacity;
    payload.length = length;
    payload.inPsram = inPsram;
    payload.snapshotMs = snapshotMs;
    payload.profilesBackedUp = buildResult.profilesBackedUp;
    return true;
}

void releaseSerializedSettingsBackupPayload(SerializedSettingsBackupPayload& payload) {
    if (payload.data != nullptr) {
        heap_caps_free(payload.data);
    }
    payload.data = nullptr;
    payload.capacity = 0;
    payload.length = 0;
    payload.inPsram = false;
    payload.protectExistingBackupFromProvisionalNvs = false;
    payload.snapshotMs = 0;
    payload.profilesBackedUp = 0;
}

bool writeBackupAtomically(fs::FS* fs, const SerializedSettingsBackupPayload& payload) {
    return writeSerializedBackupAtomically(fs, payload.data, payload.length);
}

// --- Member methods: SD backup write path ---

// Backup display/color settings to SD card

bool SettingsManager::backupToSD() {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return false; // SD not available, skip silently
    }

    // Acquire SD mutex to protect file I/O.
    // checkDmaHeap=false: backupToSD() is called from save() which runs inside
    // WiFi handlers — WiFi's SRAM buffers reduce DMA heap below the guard
    // thresholds, causing every web-UI save to silently skip the SD backup.
    // The write is small (one JSON file) and infrequent, so bypassing is safe.
    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex(), /*checkDmaHeap=*/false);
    if (!sdLock) {
        Serial.println("[Settings] Failed to acquire SD mutex for backup");
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs)
        return false;

    if (restorePending_) {
        JsonDocument existingBackup;
        const char* existingBackupPath = nullptr;
        if (loadBestBackupDocument(fs, existingBackup, &existingBackupPath, false)) {
            Serial.printf("[Settings] Restore pending; refusing to overwrite SD backup %s from provisional NVS\n",
                          existingBackupPath ? existingBackupPath : "(unknown)");
            return false;
        }
    }

    SerializedSettingsBackupPayload payload;
    if (!buildSerializedSdBackupPayload(payload, settings_, v1ProfileManager, millis())) {
        return false;
    }

    const bool ok = writeBackupAtomically(fs, payload);
    const int profilesBackedUp = payload.profilesBackedUp;
    releaseSerializedSettingsBackupPayload(payload);

    if (!ok) {
        Serial.println("[Settings] ERROR: Failed to commit SD backup atomically");
        return false;
    }

    Serial.printf("[Settings] Full backup saved to SD card (%d profiles)\n", profilesBackedUp);
    Serial.printf("[Settings] Backed up: slot0Mode=%d, slot1Mode=%d, slot2Mode=%d\n", settings_.slot0_default.mode,
                  settings_.slot1_highway.mode, settings_.slot2_comfort.mode);
    return true;
}

// ============================================================================
// Deferred SD backup writer
// ============================================================================

namespace {

constexpr UBaseType_t SETTINGS_DEFERRED_BACKUP_QUEUE_DEPTH = 1;
constexpr uint32_t SETTINGS_DEFERRED_BACKUP_WRITER_STACK_SIZE = 6144;
constexpr UBaseType_t SETTINGS_DEFERRED_BACKUP_WRITER_PRIORITY = 1;
constexpr uint32_t SETTINGS_DEFERRED_BACKUP_RETRY_BACKOFF_MS = 250;

struct DeferredSettingsBackupState {
    QueueHandle_t queue = nullptr;
    TaskHandle_t writerTask = nullptr;
    PsramQueueAllocation queueAllocation = {};
    bool queueInPsram = false;
    bool writerTaskStackInPsram = false;
    std::atomic<bool> pendingRequest{false};
    std::atomic<bool> writerRetryPending{false};
    std::atomic<bool> shutdownRequested{false};
    uint32_t nextAttemptAtMs = 0;
};

DeferredSettingsBackupState gDeferredSettingsBackupState;

bool isDeferredBackupRetryDue(uint32_t nowMs, uint32_t targetMs) {
    return static_cast<int32_t>(nowMs - targetMs) >= 0;
}

void scheduleDeferredBackupRetry(uint32_t nowMs) {
    gDeferredSettingsBackupState.pendingRequest.store(true, std::memory_order_relaxed);
    gDeferredSettingsBackupState.nextAttemptAtMs = nowMs + SETTINGS_DEFERRED_BACKUP_RETRY_BACKOFF_MS;
}

void clearDeferredBackupRetry() {
    gDeferredSettingsBackupState.nextAttemptAtMs = 0;
}

bool writeDeferredBackupPayloadNow(const SerializedSettingsBackupPayload& payload) {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        return false;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex(), /*checkDmaHeap=*/false);
    if (!lock) {
        return false;
    }

    if (payload.protectExistingBackupFromProvisionalNvs) {
        JsonDocument existingBackup;
        const char* existingBackupPath = nullptr;
        if (loadBestBackupDocument(fs, existingBackup, &existingBackupPath, false)) {
            Serial.printf(
                "[Settings] Restore pending; skipping deferred overwrite of SD backup %s from provisional NVS\n",
                existingBackupPath ? existingBackupPath : "(unknown)");
            return true;
        }
    }

    return writeBackupAtomically(fs, payload);
}

bool processDeferredBackupQueueItem(SerializedSettingsBackupPayload& payload) {
    const bool ok = writeDeferredBackupPayloadNow(payload);
    if (!ok) {
        gDeferredSettingsBackupState.writerRetryPending.store(true, std::memory_order_relaxed);
    }
    releaseSerializedSettingsBackupPayload(payload);
    return ok;
}

void deferredBackupWriterTaskEntry(void*) {
    while (true) {
        if (gDeferredSettingsBackupState.shutdownRequested.load(std::memory_order_relaxed)) {
            // Drain any queued payloads on the way out so their heap allocations
            // aren't leaked when we delete the queue from the shutdown caller.
            SerializedSettingsBackupPayload payload;
            while (xQueueReceive(gDeferredSettingsBackupState.queue, &payload, 0) == pdTRUE) {
                releaseSerializedSettingsBackupPayload(payload);
            }
            break;
        }

        SerializedSettingsBackupPayload payload;
        if (xQueueReceive(gDeferredSettingsBackupState.queue, &payload, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }

        processDeferredBackupQueueItem(payload);
        taskYIELD();
    }
    gDeferredSettingsBackupState.writerTask = nullptr;
    vTaskDeleteWithCaps(nullptr);
}

bool ensureDeferredBackupWriterReady() {
    if (gDeferredSettingsBackupState.shutdownRequested.load(std::memory_order_relaxed)) {
        // Shutdown has been requested; refuse to spin up (or respawn) the writer.
        return false;
    }

    if (gDeferredSettingsBackupState.queue == nullptr) {
        gDeferredSettingsBackupState.queue = createQueuePreferPsram(
            SETTINGS_DEFERRED_BACKUP_QUEUE_DEPTH, sizeof(SerializedSettingsBackupPayload),
            gDeferredSettingsBackupState.queueAllocation, &gDeferredSettingsBackupState.queueInPsram);
        if (gDeferredSettingsBackupState.queue == nullptr) {
            Serial.println("[Settings] ERROR: Failed to create deferred backup queue");
            return false;
        }
    }

    if (gDeferredSettingsBackupState.writerTask == nullptr) {
        const BaseType_t rc = createTaskPinnedToCoreInternalStack(
            deferredBackupWriterTaskEntry, "SettingsBackup", SETTINGS_DEFERRED_BACKUP_WRITER_STACK_SIZE, nullptr,
            SETTINGS_DEFERRED_BACKUP_WRITER_PRIORITY, &gDeferredSettingsBackupState.writerTask, 0);
        if (rc != pdPASS) {
            Serial.println("[Settings] ERROR: Failed to create deferred backup writer task");
            return false;
        }
    }

    return true;
}

bool enqueueDeferredBackupPayload(SerializedSettingsBackupPayload& payload) {
    if (gDeferredSettingsBackupState.queue == nullptr) {
        return false;
    }

    while (xQueueSend(gDeferredSettingsBackupState.queue, &payload, 0) != pdTRUE) {
        SerializedSettingsBackupPayload displaced;
        if (xQueueReceive(gDeferredSettingsBackupState.queue, &displaced, 0) != pdTRUE) {
            return false;
        }
        releaseSerializedSettingsBackupPayload(displaced);
    }

    return true;
}

} // namespace

#ifdef UNIT_TEST
void resetDeferredSettingsBackupStateForTest() {
    if (gDeferredSettingsBackupState.queue != nullptr) {
        SerializedSettingsBackupPayload payload;
        while (xQueueReceive(gDeferredSettingsBackupState.queue, &payload, 0) == pdTRUE) {
            releaseSerializedSettingsBackupPayload(payload);
        }
        vQueueDelete(gDeferredSettingsBackupState.queue);
    }
    gDeferredSettingsBackupState.queue = nullptr;
    gDeferredSettingsBackupState.writerTask = nullptr;
    if (gDeferredSettingsBackupState.queueAllocation.queueBuffer != nullptr) {
        heap_caps_free(gDeferredSettingsBackupState.queueAllocation.queueBuffer);
        gDeferredSettingsBackupState.queueAllocation.queueBuffer = nullptr;
    }
    gDeferredSettingsBackupState.queueInPsram = false;
    gDeferredSettingsBackupState.writerTaskStackInPsram = false;
    gDeferredSettingsBackupState.pendingRequest.store(false, std::memory_order_relaxed);
    gDeferredSettingsBackupState.writerRetryPending.store(false, std::memory_order_relaxed);
    gDeferredSettingsBackupState.shutdownRequested.store(false, std::memory_order_relaxed);
    gDeferredSettingsBackupState.nextAttemptAtMs = 0;
}

bool runDeferredSettingsBackupWriterOnceForTest() {
    if (gDeferredSettingsBackupState.queue == nullptr) {
        return false;
    }

    SerializedSettingsBackupPayload payload;
    if (xQueueReceive(gDeferredSettingsBackupState.queue, &payload, 0) != pdTRUE) {
        return false;
    }
    return processDeferredBackupQueueItem(payload);
}

size_t deferredSettingsBackupQueueDepthForTest() {
    if (gDeferredSettingsBackupState.queue == nullptr) {
        return 0;
    }
    return static_cast<size_t>(uxQueueMessagesWaiting(gDeferredSettingsBackupState.queue));
}

bool deferredSettingsBackupPendingForTest() {
    return gDeferredSettingsBackupState.pendingRequest.load(std::memory_order_relaxed) ||
           gDeferredSettingsBackupState.writerRetryPending.load(std::memory_order_relaxed);
}
#endif

void shutdownDeferredSettingsBackupWriter(uint32_t timeoutMs) {
    // Signal the writer task to drain and exit. The task's xQueueReceive has a
    // 1-second timeout, so a caller-supplied timeoutMs of <1s may report a
    // timeout even when the task is healthy — pass at least 1500ms when
    // possible.
    gDeferredSettingsBackupState.shutdownRequested.store(true, std::memory_order_relaxed);

    if (gDeferredSettingsBackupState.writerTask == nullptr) {
        return;
    }

    const uint32_t startMs = millis();
    while (gDeferredSettingsBackupState.writerTask != nullptr) {
        if (millis() - startMs > timeoutMs) {
            Serial.println("[Settings] Shutdown timeout waiting for deferred backup writer to exit");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool SettingsManager::saveDeferredBackup() {
    if (!persistSettingsAtomically()) {
        return false;
    }

    clearDeferredPersistState();
    bumpBackupRevision();
    Serial.println("Settings saved atomically");
    requestDeferredBackupFromCurrentState();
    return true;
}

void SettingsManager::requestDeferredBackupFromCurrentState() {
    gDeferredSettingsBackupState.pendingRequest.store(true, std::memory_order_relaxed);
    clearDeferredBackupRetry();
}

bool SettingsManager::deferredBackupPending() const {
    return gDeferredSettingsBackupState.pendingRequest.load(std::memory_order_relaxed) ||
           gDeferredSettingsBackupState.writerRetryPending.load(std::memory_order_relaxed);
}

bool SettingsManager::deferredBackupRetryScheduled() const {
    return gDeferredSettingsBackupState.nextAttemptAtMs != 0;
}

uint32_t SettingsManager::deferredBackupNextAttemptAtMs() const {
    return gDeferredSettingsBackupState.nextAttemptAtMs;
}

void SettingsManager::serviceDeferredBackup(uint32_t nowMs) {
    if (gDeferredSettingsBackupState.writerRetryPending.exchange(false, std::memory_order_relaxed)) {
        gDeferredSettingsBackupState.pendingRequest.store(true, std::memory_order_relaxed);
        clearDeferredBackupRetry();
    }

    if (!gDeferredSettingsBackupState.pendingRequest.load(std::memory_order_relaxed)) {
        return;
    }

    const uint32_t retryAtMs = gDeferredSettingsBackupState.nextAttemptAtMs;
    if (retryAtMs != 0 && !isDeferredBackupRetryDue(nowMs, retryAtMs)) {
        return;
    }

    if (!ensureDeferredBackupWriterReady()) {
        scheduleDeferredBackupRetry(nowMs);
        return;
    }

    {
        StorageManager::SDTryLock sdLock(storageManager.getSDMutex());
        if (!sdLock) {
            scheduleDeferredBackupRetry(nowMs);
            return;
        }
    }

    SerializedSettingsBackupPayload payload;
    if (!buildSerializedSdBackupPayload(payload, settings_, v1ProfileManager, nowMs)) {
        scheduleDeferredBackupRetry(nowMs);
        return;
    }
    payload.protectExistingBackupFromProvisionalNvs = restorePending_;

    if (!enqueueDeferredBackupPayload(payload)) {
        releaseSerializedSettingsBackupPayload(payload);
        scheduleDeferredBackupRetry(nowMs);
        return;
    }

    gDeferredSettingsBackupState.pendingRequest.store(false, std::memory_order_relaxed);
    clearDeferredBackupRetry();
}
