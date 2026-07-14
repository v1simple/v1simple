#pragma once

#include <stdint.h>

struct LoopPowerTouchContext {
    uint32_t nowMs = 0;
    uint32_t loopStartUs = 0;
    bool bootButtonPressed = false;
};

struct LoopPowerTouchResult {
    bool inSettings = false;
    bool shouldReturnEarly = false;
};

// Orchestrates power/touch runtime and settings-mode early return telemetry.
class LoopPowerTouchModule {
  public:
    struct Providers {
        uint32_t (*timestampUs)(void* ctx) = nullptr;
        void* timestampContext = nullptr;
        uint32_t (*microsNow)(void* ctx) = nullptr;
        void* microsContext = nullptr;

        void (*runPowerProcess)(void* ctx, uint32_t nowMs) = nullptr;
        void* powerContext = nullptr;
        bool (*runTouchUiProcess)(void* ctx, uint32_t nowMs, bool bootButtonPressed) = nullptr;
        void* touchUiContext = nullptr;

        void (*recordTouchUs)(void* ctx, uint32_t elapsedUs) = nullptr;
        void* touchPerfContext = nullptr;
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

        void (*recordHeapStats)(void* ctx, uint32_t freeHeap, uint32_t largestHeapBlock, uint32_t cachedFreeDma,
                                uint32_t cachedLargestDma) = nullptr;
        void* heapStatsContext = nullptr;
    };

    void begin(const Providers& hooks);
    LoopPowerTouchResult process(const LoopPowerTouchContext& ctx);

    // Visible for testing: settings-mode early-return heap sampling mirrors
    // LoopTelemetryModule's cadence because normal telemetry is skipped there.
    static constexpr uint8_t HEAP_SAMPLE_DIVISOR = 8;

  private:
    Providers providers{};
    uint8_t heapSampleSkip_ = HEAP_SAMPLE_DIVISOR - 1; // sample on first settings early-return
};
