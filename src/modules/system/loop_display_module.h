#pragma once

#include <stdint.h>

#include "modules/display/display_orchestration_module.h"
#include "modules/system/parsed_frame_event_module.h"

struct LoopDisplayContext {
    uint32_t nowMs = 0;
    bool bootSplashHoldActive = false;
    bool overloadLateThisLoop = false;
    bool presentationSuppressed = false;
};

// Orchestrates parsed-frame signal collection, display pipeline dispatch, and
// lightweight refresh/priority propagation.
class LoopDisplayModule {
  public:
    struct Providers {
        uint32_t (*readDisplayNowMs)(void* ctx) = nullptr;
        void* displayNowContext = nullptr;

        ParsedFrameSignal (*collectParsedSignal)(void* ctx) = nullptr;
        void* parsedSignalContext = nullptr;

        DisplayOrchestrationParsedResult (*runParsedFrame)(
            void* ctx, const DisplayOrchestrationParsedContext& parsedCtx) = nullptr;
        void* parsedFrameContext = nullptr;

        DisplayOrchestrationRefreshResult (*runLightweightRefresh)(
            void* ctx, const DisplayOrchestrationRefreshContext& refreshCtx) = nullptr;
        void* lightweightRefreshContext = nullptr;

        // Runs after the lightweight refresh decision and omits persistence
        // side effects while advancing the renderer's blink phase.
        void (*runBlinkRefresh)(void* ctx, uint32_t nowMs) = nullptr;
        void* blinkRefreshContext = nullptr;

        void (*runDisplayPipeline)(void* ctx, uint32_t nowMs) = nullptr;
        void* displayPipelineContext = nullptr;

    };

    void begin(const Providers& hooks);
    void process(const LoopDisplayContext& ctx);

  private:
    Providers providers{};
};
