/**
 * Storage Manager - SD card and LittleFS mounting
 *
 * Provides shared filesystem access for profiles, web files, and logs.
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

    // Atomic try-lock failure counter (cross-core safe for monitoring)
    static inline std::atomic<uint32_t> sdTryLockFailCount{0};

    // DMA heap starvation counter (SD ops skipped due to low internal SRAM)
    static inline std::atomic<uint32_t> sdDmaStarvationCount{0};

    // ============================================================================
    // DMA HEAP GATING - prevents SD ops when WiFi starves internal SRAM
    // ============================================================================
    // Conservative thresholds based on field evidence:
    // - WiFi uses ~50-80KB of DMA-capable internal SRAM
    // - SD_MMC needs contiguous DMA buffers for each operation
    // - Fragmentation can cause failures even with "enough" total free
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
    static bool hasDmaHeapForSD() {
        uint32_t now = millis();
        if (!dmaCache_.valid || (now - dmaCache_.lastCheckMs) > DMA_CHECK_CACHE_MS) {
            updateDmaHeapCache();
        }

        bool ok = (dmaCache_.freeDma >= MIN_DMA_FREE_FOR_SD) && (dmaCache_.largestDma >= MIN_DMA_BLOCK_FOR_SD);
        if (!ok) {
            sdDmaStarvationCount.fetch_add(1, std::memory_order_relaxed);
        }
        return ok;
    }

    // Get cached values for metrics (no API call)
    static uint32_t getCachedFreeDma() { return dmaCache_.freeDma; }
    static uint32_t getCachedLargestDma() { return dmaCache_.largestDma; }

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

    // Blocking Core 0 writer lock. DMA starvation fails before acquisition;
    // boot-time callers that require retries use SDLockBootRetry.
    class SDLockBlocking {
      public:
        explicit SDLockBlocking(SemaphoreHandle_t mutex, bool checkDmaHeap = true)
            : mutex_(mutex), acquired_(false), dmaStarved_(false) {
            // Check DMA heap first - fail fast if WiFi has starved internal SRAM
            if (checkDmaHeap && !hasDmaHeapForSD()) {
                dmaStarved_ = true;
                return;
            }
            if (mutex_) {
                acquired_ = (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE);
            }
        }
        ~SDLockBlocking() { release(); }
        bool acquired() const { return acquired_; }
        bool isDmaStarved() const { return dmaStarved_; }
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
        bool dmaStarved_;
    };

    // Startup-only blocking lock with bounded retries for DMA starvation.
    class SDLockBootRetry {
      public:
        static constexpr int MAX_RETRIES = 5;
        static constexpr int BACKOFF_MS = 100; // 100ms between retries

        explicit SDLockBootRetry(SemaphoreHandle_t mutex) : mutex_(mutex), acquired_(false), retryCount_(0) {
            for (int i = 0; i < MAX_RETRIES; ++i) {
                if (hasDmaHeapForSD()) {
                    if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
                        acquired_ = true;
                        retryCount_ = i;
                        return;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(BACKOFF_MS));
            }
            retryCount_ = MAX_RETRIES;
        }
        ~SDLockBootRetry() { release(); }
        bool acquired() const { return acquired_; }
        int retryCount() const { return retryCount_; }
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
        int retryCount_;
    };

    // There is no generic SDLock alias; each caller chooses blocking or
    // non-blocking semantics explicitly.

    // Non-blocking Core 1 lock. DMA starvation also fails immediately.
    class SDTryLock {
      public:
        explicit SDTryLock(SemaphoreHandle_t mutex, bool checkDmaHeap = true)
            : mutex_(mutex), acquired_(false), dmaStarved_(false) {
            // Check DMA heap first - fail fast if WiFi has starved internal SRAM
            if (checkDmaHeap && !hasDmaHeapForSD()) {
                dmaStarved_ = true;
                return;
            }
            if (mutex_) {
                acquired_ = (xSemaphoreTake(mutex_, 0) == pdTRUE);
                if (!acquired_) {
                    sdTryLockFailCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        ~SDTryLock() { release(); }
        bool acquired() const { return acquired_; }
        bool isDmaStarved() const { return dmaStarved_; }
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
        bool dmaStarved_;
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
