#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

class StorageManager;

class HealthCounters {
  public:
    static void reset() {
        inputDrops_.store(0, std::memory_order_relaxed);
        eventDrops_.store(0, std::memory_order_relaxed);
        eventShutdownFailures_.store(0, std::memory_order_relaxed);
        eventRetentionExhaustions_.store(0, std::memory_order_relaxed);
    }

    static void recordInputDrop(uint32_t count = 1) {
        inputDrops_.fetch_add(count, std::memory_order_relaxed);
    }

    static void recordEventDrop(uint32_t count = 1) {
        eventDrops_.fetch_add(count, std::memory_order_relaxed);
    }

    static void recordEventShutdownFailure() {
        eventShutdownFailures_.fetch_add(1, std::memory_order_relaxed);
    }

    static void recordEventRetentionExhaustion() {
        eventRetentionExhaustions_.fetch_add(1, std::memory_order_relaxed);
    }

    static uint32_t inputDrops() { return inputDrops_.load(std::memory_order_relaxed); }
    static uint32_t eventDrops() { return eventDrops_.load(std::memory_order_relaxed); }
    static uint32_t eventShutdownFailures() { return eventShutdownFailures_.load(std::memory_order_relaxed); }
    static uint32_t eventRetentionExhaustions() {
        return eventRetentionExhaustions_.load(std::memory_order_relaxed);
    }

  private:
    static inline std::atomic<uint32_t> inputDrops_{0};
    static inline std::atomic<uint32_t> eventDrops_{0};
    static inline std::atomic<uint32_t> eventShutdownFailures_{0};
    static inline std::atomic<uint32_t> eventRetentionExhaustions_{0};
};

class HealthJournal {
  public:
    static constexpr const char* kPath = "/health.log";
    static constexpr const char* kPreviousPath = "/health.prev.log";
    static constexpr uint32_t kMaxBytes = 64U * 1024U;

    bool begin(StorageManager& storage, uint32_t bootId, const char* imageId, const char* resetReason,
               bool previousClean, bool panicEvidencePresent);
    void ready(uint32_t nowMs);
    void end(uint32_t nowMs);

    bool enabled() const { return enabled_; }
    uint32_t bootId() const { return bootId_; }

#ifdef UNIT_TEST
    void resetForTest() {
        storage_ = nullptr;
        bootId_ = 0;
        enabled_ = false;
        readyWritten_ = false;
        endWritten_ = false;
    }
#endif

  private:
    bool appendLine(const char* line, size_t length);
    void disable();

    StorageManager* storage_ = nullptr;
    uint32_t bootId_ = 0;
    bool enabled_ = false;
    bool readyWritten_ = false;
    bool endWritten_ = false;
};

extern HealthJournal healthJournal;
