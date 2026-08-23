#include "loop_runtime_snapshot_module.h"

void LoopRuntimeSnapshotModule::begin(const Providers& hooks) {
    providers = hooks;
}

LoopRuntimeSnapshotValues LoopRuntimeSnapshotModule::process(const LoopRuntimeSnapshotContext& ctx) {
    (void)ctx;
    LoopRuntimeSnapshotValues values;

    if (providers.readBleConnected) {
        values.bleConnected = providers.readBleConnected(providers.bleConnectedContext);
    }

    if (providers.readDisplayPreviewRunning) {
        values.displayPreviewRunning = providers.readDisplayPreviewRunning(providers.displayPreviewContext);
    }

    return values;
}
