#include "loop_settings_prep_module.h"

void LoopSettingsPrepModule::begin(const Providers& hooks) {
    providers = hooks;
}

LoopSettingsPrepValues LoopSettingsPrepModule::process(const LoopSettingsPrepContext& ctx) {
    if (providers.runTapGesture) {
        providers.runTapGesture(providers.tapGestureContext, ctx.nowMs);
    }

    if (providers.readSettingsValues) {
        return providers.readSettingsValues(providers.settingsContext);
    }
    return LoopSettingsPrepValues{};
}
