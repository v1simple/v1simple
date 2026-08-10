#include "loop_tail_module.h"

bool LoopTailModule::begin(const Providers& hooks) {
    providers = {};
    if (!hooks.loopMicrosUs || !hooks.runBleDrain || !hooks.yieldOneTick ||
        (hooks.recordBleDrainUs && !hooks.perfTimestampUs)) {
        return false;
    }
    providers = hooks;
    return true;
}

uint32_t LoopTailModule::process(bool bleBackpressure, uint32_t loopStartUs, bool forceBleDrain) {
    if (bleBackpressure || forceBleDrain) {
        uint32_t drainStartUs = 0;
        if (providers.recordBleDrainUs) {
            drainStartUs = providers.perfTimestampUs(providers.perfTimestampContext);
        }

        providers.runBleDrain(providers.bleDrainContext);

        if (providers.recordBleDrainUs) {
            const uint32_t elapsedUs =
                static_cast<uint32_t>(providers.perfTimestampUs(providers.perfTimestampContext) - drainStartUs);
            providers.recordBleDrainUs(providers.bleDrainRecordContext, elapsedUs);
        }
    }

    // Intentional one-tick floor: this keeps lower-priority FreeRTOS work and
    // the idle-task TWDT feed running even when the main loop has no backlog.
    providers.yieldOneTick(providers.yieldContext);

    const uint32_t loopDurationUs =
        static_cast<uint32_t>(providers.loopMicrosUs(providers.loopMicrosContext) - loopStartUs);

    if (providers.recordLoopJitterUs) {
        providers.recordLoopJitterUs(providers.loopJitterContext, loopDurationUs);
    }

    return loopDurationUs;
}
