/**
 * Storage Manager implementation
 *
 * Mounts SD card (SDMMC) or falls back to LittleFS.
 */

#include "storage_manager.h"
#include "littlefs_mount.h"
#include <SD_MMC.h>
#include <LittleFS.h>

// Global instance
StorageManager storageManager;

StorageManager::StorageManager()
    : fs_(nullptr), ready_(false), usingSDMMC_(false), littlefsReady_(false), sdMutex_(nullptr) {
    // Create SD access mutex - critical for thread safety across cores
    sdMutex_ = xSemaphoreCreateMutex();
    if (!sdMutex_) {
        Serial.println("[Storage] CRITICAL: Failed to create SD mutex!");
    }
}

bool StorageManager::begin() {
    ready_ = false;
    usingSDMMC_ = false;
    littlefsReady_ = false;

#if defined(DISPLAY_WAVESHARE_349)
    // Try SD_MMC first on Waveshare 3.49
    Serial.println("[Storage] Attempting SD_MMC mount...");

    bool pinsSet = SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN);
    if (!pinsSet) {
        Serial.println("[Storage] SD_MMC.setPins() failed");
    } else if (SD_MMC.begin("/sdcard", true)) { // 1-bit mode
        fs_ = &SD_MMC;
        ready_ = true;
        usingSDMMC_ = true;
        uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
        Serial.printf("[Storage] SD card mounted (%lluMB)\n", cardSize);

        // Also mount LittleFS as secondary for backups
        // Use begin(false) to avoid auto-formatting existing data on transient errors
        littlefsReady_ = fsmount::mountStorage();
        if (!littlefsReady_) {
            Serial.println("[Storage] WARN: LittleFS secondary mount failed - mirror backups disabled");
        }

        return true;
    } else {
        Serial.println("[Storage] SD_MMC.begin() failed");
    }
#endif

    // Fallback to LittleFS
    Serial.println("[Storage] Trying LittleFS fallback...");
    if (fsmount::mountStorage()) {
        fs_ = &LittleFS;
        ready_ = true;
        littlefsReady_ = true;
        Serial.println("[Storage] LittleFS mounted");
        return true;
    }

    Serial.println("[Storage] LittleFS fallback mount failed (storage left unchanged)");
    Serial.println("[Storage] All storage mount attempts failed!");
    return false;
}

String StorageManager::statusText() const {
    if (!ready_) {
        return "No storage available";
    }
    if (usingSDMMC_) {
        uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
        return "SD card (" + String(cardSize) + "MB)";
    }
    return "LittleFS (internal)";
}

bool StorageManager::writeJsonFileAtomic(fs::FS& fs_, const char* path, JsonDocument& doc) {
    // Ensure parent directory exists (prevents VFS fopen failures)
    if (path && path[0] == '/') {
        String parent(path);
        int slash = parent.lastIndexOf('/');
        if (slash > 0) {
            parent = parent.substring(0, slash);
            if (!fs_.exists(parent)) {
                fs_.mkdir(parent);
            }
        }
    }
    String tmpPath = String(path) + ".tmp";
    File tmp = fs_.open(tmpPath.c_str(), "w");
    if (!tmp) {
        Serial.printf("[Storage] writeJsonFileAtomic: failed to open %s\n", tmpPath.c_str());
        return false;
    }
    size_t written = serializeJson(doc, tmp);
    tmp.flush();
    tmp.close();
    if (written == 0) {
        Serial.printf("[Storage] writeJsonFileAtomic: wrote 0 bytes to %s\n", tmpPath.c_str());
        fs_.remove(tmpPath.c_str());
        return false;
    }

    if (!promoteTempFileWithRollback(fs_, tmpPath.c_str(), path)) {
        Serial.printf("[Storage] writeJsonFileAtomic: promote failed %s -> %s\n", tmpPath.c_str(), path);
        return false;
    }
    return true;
}
