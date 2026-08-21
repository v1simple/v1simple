#pragma once

#include <stdint.h>

class SystemEventBus;

struct ParsedFrameSignal {
    bool parsedReady = false;
    uint32_t parsedTsMs = 0;
};

// Merges the BLE queue's authoritative parsed edge with ALP display edges.
class ParsedFrameEventModule {
  public:
    static ParsedFrameSignal collect(bool queueParsedReady, uint32_t queueParsedTsMs, SystemEventBus& eventBus);
};
