#include "loop_pre_ingest_module.h"

void LoopPreIngestModule::begin(const Providers& hooks) {
    providers = hooks;
}

LoopPreIngestResult LoopPreIngestModule::process(const LoopPreIngestContext& ctx) {
    LoopPreIngestResult result;
    result.bootReady = ctx.bootReady;

    if (!result.bootReady && ctx.nowMs >= ctx.bootReadyDeadlineMs) {
        result.bootReady = true;
        result.bootReadyOpenedByTimeout = true;
        if (providers.openBootReadyGate) {
            providers.openBootReadyGate(providers.bootReadyContext, ctx.nowMs);
        }
    }

    if (providers.runWifiPriorityApply) {
        providers.runWifiPriorityApply(
            providers.wifiPriorityContext,
            ctx.nowMs);
    }

    result.runBleProcessThisLoop = true;

    return result;
}
