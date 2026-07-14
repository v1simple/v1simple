#include "loop_runtime_snapshot_module.h"

void LoopRuntimeSnapshotModule::begin(const Providers& hooks) {
    providers = hooks;
    hasCachedCanStartDma_ = false;
    cachedCanStartDma_ = false;
}

LoopRuntimeSnapshotValues LoopRuntimeSnapshotModule::process(const LoopRuntimeSnapshotContext& ctx) {
    (void)ctx;
    LoopRuntimeSnapshotValues values;

    if (providers.readBleConnected) {
        values.bleConnected = providers.readBleConnected(providers.bleConnectedContext);
    }

    if (providers.readCanStartDma) {
        if (ctx.canStartDmaProbeAllowed) {
            values.canStartDma = providers.readCanStartDma(providers.canStartDmaContext);
            cachedCanStartDma_ = values.canStartDma;
            hasCachedCanStartDma_ = true;
        } else if (hasCachedCanStartDma_) {
            values.canStartDma = cachedCanStartDma_;
        }
    }

    if (providers.readDisplayPreviewRunning) {
        values.displayPreviewRunning = providers.readDisplayPreviewRunning(providers.displayPreviewContext);
    }

    return values;
}
