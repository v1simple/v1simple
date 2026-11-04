#pragma once
#include "settings.h"
#include "display_presentation.h"

// A synchronous explicit input for leaf-renderer tests. The returned value owns
// its snapshot; context() may be borrowed only through the current expression.
inline PresentationInputs testDisplayInputs(const SettingsManager& settings,
                                            PresentationStatusSnapshot status = {}) {
    const V1Settings& effective = settings.get();
    return {status, {effective, settings.displayConfigurationRevision(),
                    settings.getSlotAlertPersistSec(effective.activeSlot),
                    settings.getSlotPriorityArrowOnly(effective.activeSlot)},
            static_cast<uint32_t>(millis())};
}
