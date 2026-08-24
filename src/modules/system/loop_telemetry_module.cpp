#include "loop_telemetry_module.h"

void LoopTelemetryModule::begin(const Providers& hooks) {
    providers = hooks;
}

void LoopTelemetryModule::process() {
    // One ESP.getFreeHeap() call, feeding only the min-watermark. Sub-second
    // cadence is more than enough.
    if (++heapSampleSkip_ < HEAP_SAMPLE_DIVISOR) {
        return;
    }
    heapSampleSkip_ = 0;

    if (providers.recordHeapStats && providers.readFreeHeap) {
        providers.recordHeapStats(providers.heapStatsContext, providers.readFreeHeap(providers.freeHeapContext));
    }
}
