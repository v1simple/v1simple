#include "loop_power_touch_module.h"

void LoopPowerTouchModule::begin(const Providers& hooks) {
    providers = hooks;
}

LoopPowerTouchResult LoopPowerTouchModule::process(const LoopPowerTouchContext& ctx) {
    LoopPowerTouchResult result;

    if (providers.runPowerProcess) {
        providers.runPowerProcess(providers.powerContext, ctx.nowMs);
    }

    result.presentationSuppressed =
        providers.readPresentationSuppressed && providers.readPresentationSuppressed(providers.presentationContext);

    if (providers.runTouchUiProcess && !result.presentationSuppressed) {
        result.inSettings = providers.runTouchUiProcess(providers.touchUiContext, ctx.nowMs, ctx.bootButtonPressed);
    }

    if (!result.inSettings) {
        return result;
    }

    result.shouldReturnEarly = true;

    return result;
}
