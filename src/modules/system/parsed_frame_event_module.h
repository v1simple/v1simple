#pragma once

#include <stdint.h>

class SystemEventBus;

struct ParsedFrameSignal {
    bool parsedReady = false;
    uint32_t parsedTsMs = 0;
};

// Merges queue parsed-signal state with BLE_FRAME_PARSED events from SystemEventBus.
class ParsedFrameEventModule {
public:
    static ParsedFrameSignal collect(bool queueParsedReady,
                                     uint32_t queueParsedTsMs,
                                     SystemEventBus& eventBus);
};
