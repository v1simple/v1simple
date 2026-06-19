#include "wifi_heap_guard_module.h"

namespace {
const char* runtimeModeLabel(bool dualRadioMode, bool staRadioOn) {
    if (dualRadioMode) {
        return "AP+STA";
    }
    return staRadioOn ? "STA" : "AP";
}
}  // namespace

WifiHeapGuardResult WifiHeapGuardModule::evaluate(const WifiHeapGuardInput& input) const {
    WifiHeapGuardResult result;
    result.modeLabel = runtimeModeLabel(input.dualRadioMode, input.staRadioOn);

    result.freeLow = (input.freeInternal < input.criticalFree);
    if (result.freeLow && input.dualRadioMode) {
        const uint32_t freeDeficit = input.criticalFree - input.freeInternal;
        if (freeDeficit <= input.apStaFreeJitterTolerance) {
            result.freeLow = false;
        }
    }

    result.blockLow = (input.largestInternal < input.criticalBlock);
    if (result.blockLow && input.staOnlyMode) {
        const uint32_t blockDeficit = input.criticalBlock - input.largestInternal;
        if (blockDeficit <= input.staOnlyBlockJitterTolerance) {
            result.blockLow = false;
        }
    }

    result.lowHeap = result.freeLow || result.blockLow;
    return result;
}
