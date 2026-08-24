#include "loop_ingest_module.h"

bool LoopIngestModule::begin(const Providers& hooks) {
    providers = {};
    if (!hooks.runBleProcess || !hooks.runBleDrain || !hooks.readBleBackpressure) {
        return false;
    }
    providers = hooks;
    return true;
}

LoopIngestResult LoopIngestModule::process(const LoopIngestContext& ctx) {
    LoopIngestResult result;

    if (ctx.bleProcessEnabled) {
        providers.runBleProcess(providers.bleProcessContext);
    }

    providers.runBleDrain(providers.bleDrainContext);

    result.bleBackpressure = providers.readBleBackpressure(providers.bleBackpressureContext);
    result.skipLateNonCoreThisLoop = ctx.skipNonCoreThisLoop || result.bleBackpressure;
    result.overloadLateThisLoop = ctx.overloadThisLoop || result.bleBackpressure;

    return result;
}
