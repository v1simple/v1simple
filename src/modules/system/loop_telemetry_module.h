#pragma once

#include <stdint.h>

// Owns per-loop telemetry sampling and heap/cache perf recording.
class LoopTelemetryModule {
public:
    struct Providers {
        uint32_t (*microsNow)(void* ctx) = nullptr;
        void* microsContext = nullptr;
        void (*recordLoopJitterUs)(void* ctx, uint32_t jitterUs) = nullptr;
        void* loopJitterContext = nullptr;

        void (*refreshDmaCache)(void* ctx) = nullptr;
        void* dmaCacheContext = nullptr;

        uint32_t (*readFreeHeap)(void* ctx) = nullptr;
        void* freeHeapContext = nullptr;
        uint32_t (*readLargestHeapBlock)(void* ctx) = nullptr;
        void* largestHeapBlockContext = nullptr;
        uint32_t (*readCachedFreeDma)(void* ctx) = nullptr;
        void* cachedFreeDmaContext = nullptr;
        uint32_t (*readCachedLargestDma)(void* ctx) = nullptr;
        void* cachedLargestDmaContext = nullptr;
        void (*recordHeapStats)(
            void* ctx, uint32_t freeHeap, uint32_t largestHeapBlock, uint32_t cachedFreeDma, uint32_t cachedLargestDma) = nullptr;
        void* heapStatsContext = nullptr;
    };

    void begin(const Providers& hooks);
    void process(uint32_t loopStartUs);

    // Visible for testing: how often heap is sampled (every Nth loop).
    static constexpr uint8_t HEAP_SAMPLE_DIVISOR = 8;

private:
    Providers providers{};
    uint8_t heapSampleSkip_ = HEAP_SAMPLE_DIVISOR - 1;  // sample on first call
};
