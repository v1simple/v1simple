#pragma once
#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include <FS.h>
#include "../../src/storage_path_status.h"
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

        const StoragePathStatus liveStatus = storagePathStatus(fs_, livePath);
        if (liveStatus == StoragePathStatus::Unavailable) return false;
        const bool liveExists = liveStatus == StoragePathStatus::Present;
        if (liveExists) {
            if (!storageRemoveFileIfPresent(fs_, backupPathToUse)) return false;
            if (!fs_.rename(livePath, backupPathToUse)) {
                fs_.remove(tempPath);
                return false;
            }
        }

        if (!fs_.rename(tempPath, livePath)) {
            if (liveExists && storagePathStatus(fs_, livePath) == StoragePathStatus::Missing) {
                if (!fs_.rename(backupPathToUse, livePath)) {
                    Serial.printf("[Storage] promoteTempFileWithRollback: rollback failed %s -> %s\n", backupPathToUse,
                                  livePath);
                }
            }
            fs_.remove(tempPath);
            return false;
        }

        // Promotion is already committed. An unsuccessful cleanup may leave a
        // recovery copy, but must not report the committed write as a failure.
        storageRemoveFileIfPresent(fs_, backupPathToUse);

        return true;
    }

    struct MockSdLockState {
        uint32_t blockingAcquireCalls;
        uint32_t tryAcquireCalls;
        uint32_t failNextTryLockCount;
        bool failNextBlockingLock;
    };

    static inline MockSdLockState mockSdLockState{0, 0, 0, false};

    static void resetMockSdLockState() {
        mockSdLockState = MockSdLockState{0, 0, 0, false};
    }

    static bool hasDmaHeapForSD() { return true; }
    static void updateDmaHeapCache() {}

    class SDLockBlocking {
    public:
        explicit SDLockBlocking(SemaphoreHandle_t mutex, bool /*checkDmaHeap*/ = false)
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
        explicit SDTryLock(SemaphoreHandle_t mutex, bool /*checkDmaHeap*/ = false)
            : mutex_(mutex), acquired_(false) {
            StorageManager::mockSdLockState.tryAcquireCalls++;
            if (StorageManager::mockSdLockState.failNextTryLockCount > 0) {
                StorageManager::mockSdLockState.failNextTryLockCount--;
                acquired_ = false;
                return;
            }
            acquired_ = !mutex_ || xSemaphoreTake(mutex_, 0) == pdTRUE;
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
        SemaphoreHandle_t mutex_ = nullptr;
        bool acquired_ = false;
    };

    class SDLockTimed {
    public:
        SDLockTimed(SemaphoreHandle_t mutex, uint32_t timeoutMs, bool /*checkDmaHeap*/ = false)
            : mutex_(mutex), acquired_(false) {
            StorageManager::mockSdLockState.blockingAcquireCalls++;
            if (StorageManager::mockSdLockState.failNextBlockingLock) {
                StorageManager::mockSdLockState.failNextBlockingLock = false;
                return;
            }
            acquired_ = !mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
        }

        ~SDLockTimed() { release(); }
        bool acquired() const { return acquired_; }
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

inline StorageManager storage;

#endif  // STORAGE_MANAGER_H
