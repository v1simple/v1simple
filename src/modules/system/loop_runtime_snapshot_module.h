#pragma once

#include <stdint.h>

struct LoopRuntimeSnapshotValues {
    bool bleConnected = false;
    bool canStartDma = false;
    bool displayPreviewRunning = false;
};

struct LoopRuntimeSnapshotContext {
    bool canStartDmaProbeAllowed = true;
};

// Snapshots loop-local runtime service state once per iteration.
class LoopRuntimeSnapshotModule {
public:
    struct Providers {
        bool (*readBleConnected)(void* ctx) = nullptr;
        void* bleConnectedContext = nullptr;

        bool (*readCanStartDma)(void* ctx) = nullptr;
        void* canStartDmaContext = nullptr;

        bool (*readDisplayPreviewRunning)(void* ctx) = nullptr;
        void* displayPreviewContext = nullptr;
    };

    void begin(const Providers& hooks);
    LoopRuntimeSnapshotValues process(const LoopRuntimeSnapshotContext& ctx);

private:
    Providers providers{};
    bool hasCachedCanStartDma_ = false;
    bool cachedCanStartDma_ = false;
};
