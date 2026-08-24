#pragma once

#include <stdint.h>

struct LoopPowerTouchContext {
    uint32_t nowMs = 0;
    bool bootButtonPressed = false;
};

struct LoopPowerTouchResult {
    bool inSettings = false;
    bool shouldReturnEarly = false;
    bool presentationSuppressed = false;
};

// Orchestrates power/touch runtime and settings-mode early return behavior.
class LoopPowerTouchModule {
  public:
    struct Providers {
        void (*runPowerProcess)(void* ctx, uint32_t nowMs) = nullptr;
        void* powerContext = nullptr;
        bool (*readPresentationSuppressed)(void* ctx) = nullptr;
        void* presentationContext = nullptr;
        bool (*runTouchUiProcess)(void* ctx, uint32_t nowMs, bool bootButtonPressed) = nullptr;
        void* touchUiContext = nullptr;

    };

    void begin(const Providers& hooks);
    LoopPowerTouchResult process(const LoopPowerTouchContext& ctx);

  private:
    Providers providers{};
};
