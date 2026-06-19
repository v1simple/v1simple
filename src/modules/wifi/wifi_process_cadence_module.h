#pragma once

#include <stdint.h>

struct WifiProcessCadenceContext {
    uint32_t nowProcessUs = 0;
    uint32_t minIntervalUs = 2000;
};

struct WifiProcessCadenceDecision {
    bool shouldRunProcess = false;
};

// Owns WiFi process cadence state so loop() has no static gate variables.
class WifiProcessCadenceModule {
public:
    void reset();
    WifiProcessCadenceDecision process(const WifiProcessCadenceContext& ctx);

private:
    uint32_t lastProcessUs_ = 0;
};
