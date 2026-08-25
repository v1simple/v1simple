#pragma once

#include <stdint.h>

class SystemEventBus;

struct ParsedFrameSignal {
    bool parsedReady = false;
};

// Merges the BLE queue's authoritative parsed edge with ALP display edges.
class ParsedFrameEventModule {
  public:
    static ParsedFrameSignal collect(bool queueParsedReady, SystemEventBus& eventBus);
};
