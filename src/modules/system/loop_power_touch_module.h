#pragma once

#include <stdint.h>

struct LoopPowerTouchContext {
    uint32_t nowMs = 0;
    bool bootButtonPressed = false;
};

struct LoopPowerTouchResult {
    bool inSettings = false;
    bool shouldReturnEarly = false;
    bool presentationSuppressed = false;
};

// Orchestrates power/touch runtime and settings-mode early return telemetry.
class LoopPowerTouchModule {
  public:
    struct Providers {
        uint32_t (*timestampUs)(void* ctx) = nullptr;
        void* timestampContext = nullptr;

        void (*runPowerProcess)(void* ctx, uint32_t nowMs) = nullptr;
        void* powerContext = nullptr;
        bool (*readPresentationSuppressed)(void* ctx) = nullptr;
        void* presentationContext = nullptr;
        bool (*runTouchUiProcess)(void* ctx, uint32_t nowMs, bool bootButtonPressed) = nullptr;
        void* touchUiContext = nullptr;

        void (*recordTouchUs)(void* ctx, uint32_t elapsedUs) = nullptr;
        void* touchPerfContext = nullptr;

        // Cached-DMA providers removed alongside LoopTelemetryModule's; see the
        // note there. This phase mirrored that module's refresh, so it was the
        // second cross-core writer of StorageManager::dmaCache_.
        uint32_t (*readFreeHeap)(void* ctx) = nullptr;
        void* freeHeapContext = nullptr;
        uint32_t (*readLargestHeapBlock)(void* ctx) = nullptr;
        void* largestHeapBlockContext = nullptr;

        void (*recordHeapStats)(void* ctx, uint32_t freeHeap, uint32_t largestHeapBlock) = nullptr;
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
