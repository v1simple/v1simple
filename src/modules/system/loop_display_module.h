#pragma once

#include <stdint.h>

#include "modules/display/display_orchestration_module.h"
#include "modules/system/parsed_frame_event_module.h"

struct LoopDisplayContext {
    uint32_t nowMs = 0;
    bool bootSplashHoldActive = false;
    bool overloadLateThisLoop = false;
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

        // D2 fix: dispatched after runLightweightRefresh() when the orchestrator
        // signals runBlinkRefresh=true. Re-renders without persistence side
        // effects so the renderer's blink phase advances at ~96 ms even
        // when V1 packet cadence is slower.
        void (*runBlinkRefresh)(void* ctx, uint32_t nowMs) = nullptr;
        void* blinkRefreshContext = nullptr;

        void (*runDisplayPipeline)(void* ctx, uint32_t nowMs) = nullptr;
        void* displayPipelineContext = nullptr;

        uint32_t (*timestampUs)(void* ctx) = nullptr;
        void* timestampContext = nullptr;

        void (*recordDispPipeUs)(void* ctx, uint32_t elapsedUs) = nullptr;
        void* dispPipePerfContext = nullptr;

        void (*recordNotifyToDisplayMs)(void* ctx, uint32_t elapsedMs) = nullptr;
        void* notifyPerfContext = nullptr;
    };

    void begin(const Providers& hooks);
    void process(const LoopDisplayContext& ctx);

  private:
    Providers providers{};
};
