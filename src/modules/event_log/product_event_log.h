#pragma once

#include <Arduino.h>
#include <FS.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "product_event.h"

class StorageManager;

class ProductEventLog {
  public:
    static constexpr size_t kQueueCapacity = 8;
    static constexpr size_t kQueueStorageBytes = kQueueCapacity * sizeof(ProductEvent);
    static constexpr uint32_t kWriterStackBytes = 4096;
    static constexpr uint32_t kMaxFiles = 20;
    static constexpr uint32_t kMaxTotalBytes = 2U * 1024U * 1024U;

    bool begin(uint32_t bootId, StorageManager& storage);

    void observeV1Table(const AlertData* alerts, size_t count, uint8_t priorityIndex, uint32_t nowMs);
    void observeV1Link(bool connected, uint32_t nowMs);
    void observeAlp(const AlpProductObservation& observation, uint32_t nowMs);

    // Emit any active END records, stop producer admission, and wait only up
    // to timeoutMs for the one writer to drain and flush.
    void stopAndFlush(uint32_t nowMs, uint32_t timeoutMs);

    bool enabled() const { return enabled_.load(std::memory_order_acquire); }
    bool accepting() const { return accepting_.load(std::memory_order_acquire); }

#ifdef UNIT_TEST
    bool enqueueForTest(const ProductEvent& event) { return enqueue(event); }
    size_t queuedForTest() const { return queue_ ? uxQueueMessagesWaiting(queue_) : 0; }
    bool drainOneForTest();
    bool takeGapForTest(ProductEvent& event) { return takeGap(event); }
    void resetForTest() {
        accepting_.store(false, std::memory_order_relaxed);
        enabled_.store(false, std::memory_order_relaxed);
        taskRunning_.store(false, std::memory_order_relaxed);
        if (eventFile_) {
            eventFile_.close();
        }
        if (queue_) {
            vQueueDelete(queue_);
            queue_ = nullptr;
        }
    }
#endif

  private:
    static bool emitFromBuilder(const ProductEvent& event, void* context);
    static void writerTaskEntry(void* context);

    bool enqueue(const ProductEvent& event);
    void writerLoop();
    bool writeEvent(const ProductEvent& event);
    bool writeRowsLocked(const ProductEvent& event);
    bool ensureFileOpenLocked();
    bool flushIfDue(bool force);
    bool takeGap(ProductEvent& event);
    void noteDrop(uint32_t nowMs);
    void disableWriter();
    bool pruneRetention();

    StorageManager* storage_ = nullptr;
    uint32_t bootId_ = 0;
    char eventPath_[64] = {};
    ProductEventBuilder builder_;

    StaticQueue_t queueControl_{};
    alignas(ProductEvent) uint8_t queueStorage_[kQueueStorageBytes] = {};
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t writerTask_ = nullptr;

    std::atomic<bool> enabled_{false};
    std::atomic<bool> accepting_{false};
    std::atomic<bool> shutdownRequested_{false};
    std::atomic<bool> taskRunning_{false};
    std::atomic<uint32_t> pendingGapCount_{0};
    std::atomic<uint32_t> pendingGapFirstMs_{0};
    std::atomic<uint32_t> pendingGapLastMs_{0};

    File eventFile_;
    bool dirty_ = false;
    uint32_t lastFlushMs_ = 0;
    uint32_t gapSequence_ = 0;
};

static_assert(ProductEventLog::kQueueStorageBytes <= 2048, "product event queue exceeds the 2 KiB budget");
static_assert(ProductEventLog::kWriterStackBytes <= 4096, "product event writer stack exceeds the 4 KiB budget");
static_assert(ProductEventLog::kMaxFiles > 1, "event retention needs one prior-file slot and one active-file slot");

extern ProductEventLog productEventLog;
