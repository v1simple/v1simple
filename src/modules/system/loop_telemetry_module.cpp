#include "loop_telemetry_module.h"

void LoopTelemetryModule::begin(const Providers& hooks) {
    providers = hooks;
}

void LoopTelemetryModule::process() {
    // Heap introspection is expensive (two heap_caps calls, down from four).
    // The data only feeds min-watermark tracking, so sub-second cadence is more
    // than enough.
    if (++heapSampleSkip_ < HEAP_SAMPLE_DIVISOR) {
        return;
    }
    heapSampleSkip_ = 0;

    if (providers.recordHeapStats && providers.readFreeHeap && providers.readLargestHeapBlock) {
        providers.recordHeapStats(providers.heapStatsContext, providers.readFreeHeap(providers.freeHeapContext),
                                  providers.readLargestHeapBlock(providers.largestHeapBlockContext));
    }
}
