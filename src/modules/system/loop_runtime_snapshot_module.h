#pragma once

#include <stdint.h>

struct LoopRuntimeSnapshotValues {
    bool bleConnected = false;
    bool displayPreviewRunning = false;
};

struct LoopRuntimeSnapshotContext {};

// Snapshots loop-local runtime service state once per iteration.
class LoopRuntimeSnapshotModule {
  public:
    struct Providers {
        bool (*readBleConnected)(void* ctx) = nullptr;
        void* bleConnectedContext = nullptr;

        bool (*readDisplayPreviewRunning)(void* ctx) = nullptr;
        void* displayPreviewContext = nullptr;
    };

    void begin(const Providers& hooks);
    LoopRuntimeSnapshotValues process(const LoopRuntimeSnapshotContext& ctx);

  private:
    Providers providers{};
};
