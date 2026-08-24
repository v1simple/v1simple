#pragma once

#include <stdint.h>

struct LoopIngestContext {
    uint32_t nowMs = 0;
    bool bleProcessEnabled = false;
    bool skipNonCoreThisLoop = false;
    bool overloadThisLoop = false;
};

struct LoopIngestResult {
    bool bleBackpressure = false;
    bool skipLateNonCoreThisLoop = false;
    bool overloadLateThisLoop = false;
};

// Orchestrates BLE ingest and backpressure merge.
class LoopIngestModule {
  public:
    struct Providers {
        void (*runBleProcess)(void* ctx) = nullptr;
        void* bleProcessContext = nullptr;

        void (*runBleDrain)(void* ctx) = nullptr;
        void* bleDrainContext = nullptr;
        bool (*readBleBackpressure)(void* ctx) = nullptr;
        void* bleBackpressureContext = nullptr;
    };

    bool begin(const Providers& hooks);
    LoopIngestResult process(const LoopIngestContext& ctx);

  private:
    Providers providers{};
};
