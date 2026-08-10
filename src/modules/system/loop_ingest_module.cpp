#include "loop_ingest_module.h"

bool LoopIngestModule::begin(const Providers& hooks) {
    providers = {};
    if (!hooks.runBleProcess || !hooks.runBleDrain || !hooks.readBleBackpressure ||
        (hooks.recordBleProcessUs && !hooks.timestampUs) || (hooks.recordBleDrainUs && !hooks.timestampUs)) {
        return false;
    }
    providers = hooks;
    return true;
}

LoopIngestResult LoopIngestModule::process(const LoopIngestContext& ctx) {
    LoopIngestResult result;

    if (ctx.bleProcessEnabled) {
        if (providers.recordBleProcessUs) {
            const uint32_t startUs = providers.timestampUs(providers.timestampContext);
            providers.runBleProcess(providers.bleProcessContext);
            providers.recordBleProcessUs(providers.bleProcessPerfContext,
                                         providers.timestampUs(providers.timestampContext) - startUs);
        } else {
            providers.runBleProcess(providers.bleProcessContext);
        }
    }

    if (providers.recordBleDrainUs) {
        const uint32_t startUs = providers.timestampUs(providers.timestampContext);
        providers.runBleDrain(providers.bleDrainContext);
        providers.recordBleDrainUs(providers.bleDrainPerfContext,
                                   providers.timestampUs(providers.timestampContext) - startUs);
    } else {
        providers.runBleDrain(providers.bleDrainContext);
    }

    result.bleBackpressure = providers.readBleBackpressure(providers.bleBackpressureContext);
    result.skipLateNonCoreThisLoop = ctx.skipNonCoreThisLoop || result.bleBackpressure;
    result.overloadLateThisLoop = ctx.overloadThisLoop || result.bleBackpressure;

    return result;
}
