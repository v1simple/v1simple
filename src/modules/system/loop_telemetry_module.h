#pragma once

#include <stdint.h>

// Owns per-loop telemetry sampling and heap/cache perf recording.
class LoopTelemetryModule {
  public:
    // The cached-DMA half of this module is gone. It refreshed
    // StorageManager::dmaCache_ from Core 1 and fed the freeDma / largestDma /
    // freeDmaMin / largestDmaMin CSV columns, all of which reported the
    // MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT pool. Those columns were near-duplicates
    // of the freeDmaCap / largestDmaCap pair that perf_snapshot samples directly
    // (largestDma matched largestDmaCap in 42,374 of 42,437 bench rows), and the
    // refresh was the only cross-core writer of a non-atomic cache. Removing it
    // leaves dmaCache_ single-threaded, written only by hasDmaHeapForSD() on
    // Core 1.
    struct Providers {
        uint32_t (*readFreeHeap)(void* ctx) = nullptr;
        void* freeHeapContext = nullptr;
        void (*recordHeapStats)(void* ctx, uint32_t freeHeap) = nullptr;
        void* heapStatsContext = nullptr;
    };

    void begin(const Providers& hooks);
    void process();

    // Visible for testing: how often heap is sampled (every Nth loop).
    static constexpr uint8_t HEAP_SAMPLE_DIVISOR = 8;

  private:
    Providers providers{};
    uint8_t heapSampleSkip_ = HEAP_SAMPLE_DIVISOR - 1; // sample on first call
};
