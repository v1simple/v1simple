#pragma once
#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include <FS.h>
#include <atomic>

#include "Arduino.h"
#include "freertos/semphr.h"

class StorageManager {
public:
    StorageManager() = default;

    bool begin() { return ready_; }

    void reset() {
        ready_ = false;
        usingSdCard_ = false;
        littlefsReady_ = false;
        filesystem_ = nullptr;
        littlefs_ = nullptr;
        if (!sdMutex_) {
            sdMutex_ = xSemaphoreCreateMutex();
        }
    }

    void setFilesystem(fs::FS* filesystem, bool useSdCard = true) {
        filesystem_ = filesystem;
        ready_ = filesystem != nullptr;
        usingSdCard_ = ready_ && useSdCard;
    }

    void setLittleFS(fs::FS* filesystem) {
        littlefs_ = filesystem;
        littlefsReady_ = filesystem != nullptr;
    }

    bool isReady() const { return ready_; }
    bool isSDCard() const { return usingSdCard_; }
    bool isLittleFSReady() const { return littlefsReady_; }
    String statusText() const { return ready_ ? (usingSdCard_ ? "SD" : "FS") : "offline"; }

    fs::FS* getFilesystem() const { return filesystem_; }
    fs::FS* getLittleFS() const { return littlefsReady_ ? littlefs_ : nullptr; }
    SemaphoreHandle_t getSDMutex() const { return sdMutex_; }

    static String rollbackPathFor(const char* livePath) {
        if (!livePath || livePath[0] == '\0') {
            return String("");
        }
        return String(livePath) + ".prev";
    }

    static bool promoteTempFileWithRollback(fs::FS& fs, const char* tempPath, const char* livePath) {
        if (!tempPath || !livePath) {
            return false;
        }
        const String backupPath = rollbackPathFor(livePath);
        const bool hadLive = fs.exists(livePath);
        if (hadLive && !fs.rename(livePath, backupPath.c_str())) {
            return false;
        }
        if (!fs.rename(tempPath, livePath)) {
            if (hadLive) {
                fs.rename(backupPath.c_str(), livePath);
            }
            return false;
        }
        if (hadLive) {
            fs.remove(backupPath.c_str());
        }
        return true;
    }

    static inline std::atomic<uint32_t> sdTryLockFailCount{0};
    static inline std::atomic<uint32_t> sdDmaStarvationCount{0};

    struct MockSdLockState {
        uint32_t blockingAcquireCalls;
        uint32_t tryAcquireCalls;
        uint32_t failNextTryLockCount;
        bool failNextBlockingLock;
    };

    static inline MockSdLockState mockSdLockState{0, 0, 0, false};

    static void resetMockSdLockState() {
        mockSdLockState = MockSdLockState{0, 0, 0, false};
        sdTryLockFailCount.store(0);
    }

    static bool hasDmaHeapForSD() { return true; }
    static uint32_t getCachedFreeDma() { return 65536; }
    static uint32_t getCachedLargestDma() { return 65536; }
    static void updateDmaHeapCache() {}

    class SDLockBlocking {
    public:
        explicit SDLockBlocking(SemaphoreHandle_t mutex, bool /*checkDmaHeap*/ = true)
            : mutex_(mutex), acquired_(false) {
            StorageManager::mockSdLockState.blockingAcquireCalls++;
            if (StorageManager::mockSdLockState.failNextBlockingLock) {
                StorageManager::mockSdLockState.failNextBlockingLock = false;
                acquired_ = false;
                return;
            }
            acquired_ = !mutex_ || xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
        }

        ~SDLockBlocking() { release(); }

        bool acquired() const { return acquired_; }
        bool isDmaStarved() const { return false; }
        operator bool() const { return acquired_; }

        void release() {
            if (acquired_ && mutex_) {
                xSemaphoreGive(mutex_);
                acquired_ = false;
            }
        }

    private:
        SemaphoreHandle_t mutex_ = nullptr;
        bool acquired_ = false;
    };

    class SDTryLock {
    public:
        explicit SDTryLock(SemaphoreHandle_t mutex, bool /*checkDmaHeap*/ = true)
            : mutex_(mutex), acquired_(false) {
            StorageManager::mockSdLockState.tryAcquireCalls++;
            if (StorageManager::mockSdLockState.failNextTryLockCount > 0) {
                StorageManager::mockSdLockState.failNextTryLockCount--;
                StorageManager::sdTryLockFailCount.fetch_add(1);
                acquired_ = false;
                return;
            }
            acquired_ = !mutex_ || xSemaphoreTake(mutex_, 0) == pdTRUE;
            if (!acquired_) {
                StorageManager::sdTryLockFailCount.fetch_add(1);
            }
        }

        ~SDTryLock() { release(); }

        bool acquired() const { return acquired_; }
        bool isDmaStarved() const { return false; }
        operator bool() const { return acquired_; }

        void release() {
            if (acquired_ && mutex_) {
                xSemaphoreGive(mutex_);
                acquired_ = false;
            }
        }

    private:
        SemaphoreHandle_t mutex_ = nullptr;
        bool acquired_ = false;
    };

private:
    bool ready_ = false;
    bool usingSdCard_ = false;
    bool littlefsReady_ = false;
    fs::FS* filesystem_ = nullptr;
    fs::FS* littlefs_ = nullptr;
    SemaphoreHandle_t sdMutex_ = xSemaphoreCreateMutex();
};

inline StorageManager storageManager;

#endif  // STORAGE_MANAGER_H
