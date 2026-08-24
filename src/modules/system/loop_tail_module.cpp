#include "loop_tail_module.h"

bool LoopTailModule::begin(const Providers& hooks) {
    providers = {};
    if (!hooks.loopMicrosUs || !hooks.runBleDrain || !hooks.yieldOneTick) {
        return false;
    }
    providers = hooks;
    return true;
}

uint32_t LoopTailModule::process(bool bleBackpressure, uint32_t loopStartUs, bool forceBleDrain) {
    if (bleBackpressure || forceBleDrain) {
        providers.runBleDrain(providers.bleDrainContext);
    }

    // Intentional one-tick floor: this keeps lower-priority FreeRTOS work and
    // the idle-task TWDT feed running even when the main loop has no backlog.
    providers.yieldOneTick(providers.yieldContext);

    const uint32_t loopDurationUs =
        static_cast<uint32_t>(providers.loopMicrosUs(providers.loopMicrosContext) - loopStartUs);

    return loopDurationUs;
}
