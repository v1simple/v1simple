#pragma once

#include <stdint.h>

struct WifiHeapGuardInput {
    bool dualRadioMode = false;
    bool staRadioOn = false;
    bool staOnlyMode = false;
    uint32_t freeInternal = 0;
    uint32_t largestInternal = 0;
    uint32_t criticalFree = 0;
    uint32_t criticalBlock = 0;
    uint32_t apStaFreeJitterTolerance = 0;
    uint32_t staOnlyBlockJitterTolerance = 0;
};

struct WifiHeapGuardResult {
    bool freeLow = false;
    bool blockLow = false;
    bool lowHeap = false;
    const char* modeLabel = "AP";
};

// Evaluates WiFi runtime heap pressure with mode-aware jitter tolerance.
class WifiHeapGuardModule {
public:
    WifiHeapGuardResult evaluate(const WifiHeapGuardInput& input) const;
};
