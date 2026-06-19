#pragma once

#include <stdint.h>

struct LoopPreIngestContext {
    uint32_t nowMs = 0;
    bool bootReady = false;
    uint32_t bootReadyDeadlineMs = 0;
};

struct LoopPreIngestResult {
    bool bootReady = false;
    bool runBleProcessThisLoop = false;
    bool bootReadyOpenedByTimeout = false;
};

// Orchestrates pre-ingest boot-ready timeout handling and runtime policy ticks.
class LoopPreIngestModule {
public:
    struct Providers {
        void (*openBootReadyGate)(void* ctx, uint32_t nowMs) = nullptr;
        void* bootReadyContext = nullptr;

        void (*runWifiPriorityApply)(void* ctx, uint32_t nowMs) = nullptr;
        void* wifiPriorityContext = nullptr;
    };

    void begin(const Providers& hooks);
    LoopPreIngestResult process(const LoopPreIngestContext& ctx);

private:
    Providers providers{};
};
