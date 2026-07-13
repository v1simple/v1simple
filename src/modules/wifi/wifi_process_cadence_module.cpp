#include "wifi_process_cadence_module.h"

void WifiProcessCadenceModule::reset() {
    lastProcessUs_ = 0;
}

WifiProcessCadenceDecision WifiProcessCadenceModule::process(
        const WifiProcessCadenceContext& ctx) {
    WifiProcessCadenceDecision decision;
    if (lastProcessUs_ == 0 ||
        static_cast<uint32_t>(ctx.nowProcessUs - lastProcessUs_) >= ctx.minIntervalUs) {
        decision.shouldRunProcess = true;
        lastProcessUs_ = ctx.nowProcessUs;
    }
    return decision;
}
