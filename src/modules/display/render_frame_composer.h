#pragma once

#include "render_frame.h"
#include "settings.h"

struct V1Snapshot {
    DisplayState state{};
    const AlertData* alerts = nullptr;
    int alertCount = 0;
    AlertData priority{};
    bool hasRenderablePriority = false;
    bool hasPersistedAlert = false;
    AlertData persistedAlert{};
};

struct AlpSnapshot {
    AlpLaserEvent event{};
    bool ownsLaserDisplay = false;
    bool isPersistedLatch = false;
    AlpLaserEvent latchedEvent{};
};

class RenderFrameComposer {
public:
    RenderFrame compose(const V1Snapshot& v1,
                        const AlpSnapshot& alp,
                        const V1Settings& settings,
                        uint32_t nowMs) const;
};