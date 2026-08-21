#include "loop_power_touch_module.h"

void LoopPowerTouchModule::begin(const Providers& hooks) {
    providers = hooks;
    heapSampleSkip_ = HEAP_SAMPLE_DIVISOR - 1;
}

LoopPowerTouchResult LoopPowerTouchModule::process(const LoopPowerTouchContext& ctx) {
    LoopPowerTouchResult result;

    if (providers.runPowerProcess) {
        providers.runPowerProcess(providers.powerContext, ctx.nowMs);
    }

    result.presentationSuppressed =
        providers.readPresentationSuppressed && providers.readPresentationSuppressed(providers.presentationContext);

    if (providers.runTouchUiProcess && !result.presentationSuppressed) {
        if (providers.timestampUs && providers.recordTouchUs) {
            const uint32_t startUs = providers.timestampUs(providers.timestampContext);
            result.inSettings = providers.runTouchUiProcess(providers.touchUiContext, ctx.nowMs, ctx.bootButtonPressed);
            providers.recordTouchUs(providers.touchPerfContext,
                                    providers.timestampUs(providers.timestampContext) - startUs);
        } else {
            result.inSettings = providers.runTouchUiProcess(providers.touchUiContext, ctx.nowMs, ctx.bootButtonPressed);
        }
    }

    if (!result.inSettings) {
        return result;
    }

    result.shouldReturnEarly = true;

    // The normal telemetry phase is skipped by the settings-mode early return.
    // Mirror LoopTelemetryModule's bounded heap/DMA cadence because those
    // probes are comparatively expensive. The common loop tail records the
    // complete loop duration after this phase returns.
    if (++heapSampleSkip_ < HEAP_SAMPLE_DIVISOR) {
        return result;
    }
    heapSampleSkip_ = 0;

    if (providers.refreshDmaCache) {
        providers.refreshDmaCache(providers.dmaCacheContext);
    }

    if (providers.recordHeapStats) {
        const uint32_t freeHeap = providers.readFreeHeap ? providers.readFreeHeap(providers.freeHeapContext) : 0;
        const uint32_t largestHeapBlock =
            providers.readLargestHeapBlock ? providers.readLargestHeapBlock(providers.largestHeapBlockContext) : 0;
        const uint32_t cachedFreeDma =
            providers.readCachedFreeDma ? providers.readCachedFreeDma(providers.cachedFreeDmaContext) : 0;
        const uint32_t cachedLargestDma =
            providers.readCachedLargestDma ? providers.readCachedLargestDma(providers.cachedLargestDmaContext) : 0;
        providers.recordHeapStats(providers.heapStatsContext, freeHeap, largestHeapBlock, cachedFreeDma,
                                  cachedLargestDma);
    }

    return result;
}
