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
    // to timeoutMs for the one writer to drain, flush, close, and relinquish
    // filesystem ownership. A logger that never acquired writer ownership is
    // already stopped. False means a live writer still owns storage after the
    // deadline; admission is nevertheless closed so product shutdown/restart
    // can proceed without further event-log writes.
    bool stopAndFlush(uint32_t nowMs, uint32_t timeoutMs);

    // Cancel an in-flight stop or restart the one writer after a confirmed
    // exit. Used only when a power-off sequence aborts and runtime continues.
    bool resumeAfterAbortedShutdown(uint32_t timeoutMs);

    bool enabled() const { return enabled_.load(std::memory_order_acquire); }
    bool accepting() const { return accepting_.load(std::memory_order_acquire); }
    bool writerStopped() const;

#ifdef UNIT_TEST
    bool enqueueForTest(const ProductEvent& event) { return enqueue(event); }
    size_t queuedForTest() const { return queue_ ? uxQueueMessagesWaiting(queue_) : 0; }
    bool drainOneForTest();
    bool takeGapForTest(ProductEvent& event) { return takeGap(event); }
    void runWriterForTest() { writerLoop(); }
    void requestStopForTest(uint32_t nowMs) {
        if (accepting_.load(std::memory_order_acquire)) {
            builder_.closeActive(nowMs);
        }
        accepting_.store(false, std::memory_order_release);
        WriterState state = WriterState::RUNNING;
        (void)writerState_.compare_exchange_strong(state, WriterState::STOP_REQUESTED,
                                                   std::memory_order_acq_rel);
    }
    size_t retainedBytesForTest() const { return retainedBytes_; }
    size_t activeBytesForTest() const { return activeBytes_; }
    void resetForTest() {
        accepting_.store(false, std::memory_order_relaxed);
        enabled_.store(false, std::memory_order_relaxed);
        writerState_.store(WriterState::STOPPED, std::memory_order_relaxed);
        writerOwnershipAcquired_.store(false, std::memory_order_relaxed);
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
    enum class WriterState : uint8_t { STOPPED = 0, RUNNING, STOP_REQUESTED, CLOSING, FAILED };

    static bool emitFromBuilder(const ProductEvent& event, void* context);
    static void writerTaskEntry(void* context);

    bool startWriterTask();
    bool enqueue(const ProductEvent& event);
    void writerLoop();
    bool writeEvent(const ProductEvent& event);
    bool serializedEventBytes(const ProductEvent& event, size_t& bytes) const;
    bool writeRowsLocked(const ProductEvent& event);
    bool ensureFileOpenLocked();
    bool flushIfDue(bool force);
    bool takeGap(ProductEvent& event);
    void noteDrop(uint32_t nowMs);
    void disableWriter();
    void recordStopFailure();
    void recordRetentionExhaustion(uint32_t dropped);
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
    std::atomic<WriterState> writerState_{WriterState::STOPPED};
    std::atomic<bool> writerOwnershipAcquired_{false};
    std::atomic<bool> writerExitClean_{false};
    std::atomic<bool> stopFailureRecorded_{false};
    std::atomic<bool> retentionExhausted_{false};
    std::atomic<uint32_t> pendingGapCount_{0};
    std::atomic<uint32_t> pendingGapFirstMs_{0};
    std::atomic<uint32_t> pendingGapLastMs_{0};

    File eventFile_;
    bool dirty_ = false;
    bool fileCreated_ = false;
    size_t retainedBytes_ = 0;
    size_t activeBytes_ = 0;
    uint32_t lastFlushMs_ = 0;
    uint32_t gapSequence_ = 0;
};

static_assert(ProductEventLog::kQueueStorageBytes <= 2048, "product event queue exceeds the 2 KiB budget");
static_assert(ProductEventLog::kWriterStackBytes <= 4096, "product event writer stack exceeds the 4 KiB budget");
static_assert(ProductEventLog::kMaxFiles > 1, "event retention needs one prior-file slot and one active-file slot");

extern ProductEventLog productEventLog;
