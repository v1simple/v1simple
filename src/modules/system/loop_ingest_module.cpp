#include "loop_ingest_module.h"

void LoopIngestModule::begin(const Providers& hooks) {
    providers = hooks;
}

LoopIngestResult LoopIngestModule::process(const LoopIngestContext& ctx) {
    LoopIngestResult result;

    if (ctx.bleProcessEnabled && providers.runBleProcess) {
        if (providers.timestampUs && providers.recordBleProcessUs) {
            const uint32_t startUs = providers.timestampUs(providers.timestampContext);
            providers.runBleProcess(providers.bleProcessContext);
            providers.recordBleProcessUs(
                providers.bleProcessPerfContext,
                providers.timestampUs(providers.timestampContext) - startUs);
        } else {
            providers.runBleProcess(providers.bleProcessContext);
        }
    }

    if (providers.runBleDrain) {
        if (providers.timestampUs && providers.recordBleDrainUs) {
            const uint32_t startUs = providers.timestampUs(providers.timestampContext);
            providers.runBleDrain(providers.bleDrainContext);
            providers.recordBleDrainUs(
                providers.bleDrainPerfContext,
                providers.timestampUs(providers.timestampContext) - startUs);
        } else {
            providers.runBleDrain(providers.bleDrainContext);
        }
    }

    if (providers.readBleBackpressure) {
        result.bleBackpressure = providers.readBleBackpressure(providers.bleBackpressureContext);
    }
    result.skipLateNonCoreThisLoop = ctx.skipNonCoreThisLoop || result.bleBackpressure;
    result.overloadLateThisLoop = ctx.overloadThisLoop || result.bleBackpressure;

    return result;
}
