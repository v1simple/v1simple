#pragma once

#include <stdint.h>

struct LoopRuntimeSnapshotValues {
    bool displayPreviewRunning = false;
};

struct LoopRuntimeSnapshotContext {};

// Snapshots loop-local runtime service state once per iteration.
class LoopRuntimeSnapshotModule {
  public:
    struct Providers {
        bool (*readDisplayPreviewRunning)(void* ctx) = nullptr;
        void* displayPreviewContext = nullptr;
    };

    void begin(const Providers& hooks);
    LoopRuntimeSnapshotValues process(const LoopRuntimeSnapshotContext& ctx);

  private:
    Providers providers{};
};
