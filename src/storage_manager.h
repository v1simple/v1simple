/**
 * Storage Manager - SD card and LittleFS mounting
 *
 * Provides shared filesystem access for profiles, web files, and product persistence.
 */

#pragma once
#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include <Arduino.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>

// Waveshare 3.49 SD card pins (SDMMC interface)
#ifndef SD_MMC_CLK_PIN
#define SD_MMC_CLK_PIN 41
#endif
#ifndef SD_MMC_CMD_PIN
#define SD_MMC_CMD_PIN 39
#endif
#ifndef SD_MMC_D0_PIN
#define SD_MMC_D0_PIN 40
#endif

class StorageManager {
  public:
    StorageManager();

    // Mount storage (SD card preferred, LittleFS fallback)
    bool begin();

    bool isReady() const { return ready_; }
    bool isSDCard() const { return usingSDMMC_; }
    bool isLittleFSReady() const { return littlefsReady_; }
    String statusText() const;

    // Get underlying filesystem
    fs::FS* getFilesystem() const { return fs_; }
    // Secondary LittleFS handle (available even when SD is primary)
    fs::FS* getLittleFS() const { return littlefsReady_ ? &LittleFS : nullptr; }

    // Thread-safe SD access mutex - MUST be held during all file operations
    // when multiple cores/tasks may access SD simultaneously
    SemaphoreHandle_t getSDMutex() const { return sdMutex_; }

    // ============================================================================
    // DMA HEAP GATING - prevents SD ops when WiFi starves internal SRAM
    // ============================================================================
    // Conservative thresholds based on field evidence:
    // - WiFi uses ~50-80KB of DMA-capable internal SRAM
    // - SD_MMC needs contiguous DMA buffers for each operation
    // - Fragmentation can cause failures even with "enough" total free
    //
    // WHO PAYS FOR THIS. Split boot means WiFi runs only in maintenance boot:
    // startSetupMode() has exactly two call sites, main.cpp:499 inside
    // initializeMaintenanceBootFlow() and main.cpp:641 inside the maintenance
    // loop branch, and wifiManager.process() has one, main.cpp:620 in that same
    // branch. Commit bd2435c removed the last normal-runtime autostart path.
    //
    // So only five lock sites in the tree can ever evaluate this gate with the
    // radio up:
    //
    //   settings_backup.cpp:850      serviceDeferredBackup  <- main.cpp:647
    //   settings_nvs.cpp:332/385/433/471   wifi client secrets <- wifi_client.cpp
    //
    // Those five opt in with checkDmaHeap=true. Every other site runs where
    // WiFi is structurally incapable of being on -- normal boot, or the pre-WiFi
    // part of maintenance boot -- so the default is false and they skip two heap
    // traversals per lock. Do not flip this default back without re-deriving the
    // reachability; a site that does not need the gate paying for it is the
    // whole reason it was inverted.
    static constexpr uint32_t MIN_DMA_FREE_FOR_SD = 16384; // 16KB total free
    static constexpr uint32_t MIN_DMA_BLOCK_FOR_SD = 2048; // 2KB largest block
    static constexpr uint32_t DMA_CHECK_CACHE_MS = 100;    // Cache check for 100ms

    // Cached DMA heap state (avoid repeated API calls in hot paths)
    struct DmaHeapCache {
        uint32_t freeDma;
        uint32_t largestDma;
        uint32_t lastCheckMs;
        bool valid;
    };
    static inline DmaHeapCache dmaCache_ = {0, 0, 0, false};

    // Update cached DMA heap state (call from main loop periodically)
    static void updateDmaHeapCache() {
        dmaCache_.freeDma = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        dmaCache_.largestDma = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        dmaCache_.lastCheckMs = millis();
        dmaCache_.valid = true;
    }

    // Check if there's enough DMA-capable heap for SD operations
    // Uses cached values if recent, otherwise updates cache
    // Returns false if WiFi has starved internal SRAM (free OR fragmented)
    //
    // This is now the only writer of dmaCache_, and every caller that reaches it
    // runs on the main-loop task (Core 1): the seven opt-in sites listed above
    // are driven either from the maintenance loop directly (main.cpp:647) or
    // from route handlers dispatched inside wifiManager.process() at
    // main.cpp:620. The cache is therefore single-threaded and needs no atomics.
    static bool hasDmaHeapForSD() {
        uint32_t now = millis();
        if (!dmaCache_.valid || (now - dmaCache_.lastCheckMs) > DMA_CHECK_CACHE_MS) {
            updateDmaHeapCache();
        }

        return (dmaCache_.freeDma >= MIN_DMA_FREE_FOR_SD) && (dmaCache_.largestDma >= MIN_DMA_BLOCK_FOR_SD);
    }

    // ============================================================================
    // SD access policy: two explicit lock types.
    // ============================================================================
    //
    // SDLockBlocking is limited to Core 0 writers and boot/shutdown paths.
    //
    // SDTryLock is the non-blocking Core 1 main-loop lock. Callers must skip or
    // defer work when acquisition fails.
    //
    // The BLE-to-display path must not block on best-effort SD work.
    // ============================================================================

    // Blocking Core 0 writer lock. When the DMA gate is requested, starvation
    // fails before acquisition.
    class SDLockBlocking {
      public:
        explicit SDLockBlocking(SemaphoreHandle_t mutex, bool checkDmaHeap = false)
            : mutex_(mutex), acquired_(false) {
            // Check DMA heap first - fail fast if WiFi has starved internal SRAM
            if (checkDmaHeap && !hasDmaHeapForSD()) {
                return;
            }
            if (mutex_) {
                acquired_ = (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE);
            }
        }
        ~SDLockBlocking() { release(); }
        bool acquired() const { return acquired_; }
        operator bool() const { return acquired_; }

        void release() {
            if (acquired_ && mutex_) {
                xSemaphoreGive(mutex_);
                acquired_ = false;
            }
        }

      private:
        SemaphoreHandle_t mutex_;
        bool acquired_;
    };

    // There is no generic SDLock alias; each caller chooses blocking or
    // non-blocking semantics explicitly.

    // Non-blocking Core 1 lock. DMA starvation also fails immediately.
    class SDTryLock {
      public:
        explicit SDTryLock(SemaphoreHandle_t mutex, bool checkDmaHeap = false)
            : mutex_(mutex), acquired_(false) {
            // Check DMA heap first - fail fast if WiFi has starved internal SRAM
            if (checkDmaHeap && !hasDmaHeapForSD()) {
                return;
            }
            if (mutex_) {
                acquired_ = (xSemaphoreTake(mutex_, 0) == pdTRUE);
            }
        }
        ~SDTryLock() { release(); }
        bool acquired() const { return acquired_; }
        operator bool() const { return acquired_; }

        void release() {
            if (acquired_ && mutex_) {
                xSemaphoreGive(mutex_);
                acquired_ = false;
            }
        }

      private:
        SemaphoreHandle_t mutex_;
        bool acquired_;
    };

    // Promote a temp file to the live path with rollback if promotion fails.
    // Returns true on success.
    static String rollbackPathFor(const char* livePath) {
        if (!livePath || livePath[0] == '\0') {
            return String("");
        }
        return String(livePath) + ".prev";
    }

    static bool promoteTempFileWithRollback(fs::FS& fs_, const char* tempPath, const char* livePath,
                                            const char* backupPath = nullptr) {
        if (!tempPath || tempPath[0] == '\0' || !livePath || livePath[0] == '\0') {
            return false;
        }

        String derivedBackupPath;
        const char* backupPathToUse = backupPath;
        if (!backupPathToUse || backupPathToUse[0] == '\0') {
            derivedBackupPath = rollbackPathFor(livePath);
            backupPathToUse = derivedBackupPath.c_str();
        }

        const bool liveExists = fs_.exists(livePath);
        if (liveExists) {
            if (backupPathToUse && backupPathToUse[0] != '\0' && fs_.exists(backupPathToUse)) {
                fs_.remove(backupPathToUse);
            }
            if (!fs_.rename(livePath, backupPathToUse)) {
                fs_.remove(tempPath);
                return false;
            }
        }

        if (!fs_.rename(tempPath, livePath)) {
            if (liveExists && fs_.exists(backupPathToUse) && !fs_.exists(livePath)) {
                if (!fs_.rename(backupPathToUse, livePath)) {
                    Serial.printf("[Storage] promoteTempFileWithRollback: rollback failed %s -> %s\n", backupPathToUse,
                                  livePath);
                }
            }
            fs_.remove(tempPath);
            return false;
        }

        if (backupPathToUse && backupPathToUse[0] != '\0' && fs_.exists(backupPathToUse)) {
            fs_.remove(backupPathToUse);
        }

        return true;
    }

    // Atomic JSON file write utility (write to .tmp, then promote).
    // Returns true on success.
    static bool writeJsonFileAtomic(fs::FS& fs_, const char* path, JsonDocument& doc);

  private:
    fs::FS* fs_;
    bool ready_;
    bool usingSDMMC_;
    bool littlefsReady_;
    SemaphoreHandle_t sdMutex_;
};

// Global instance
extern StorageManager storageManager;
#endif // STORAGE_MANAGER_H
