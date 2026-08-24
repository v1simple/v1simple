#include "loop_display_module.h"

void LoopDisplayModule::begin(const Providers& hooks) {
    providers = hooks;
}

void LoopDisplayModule::process(const LoopDisplayContext& ctx) {
    const uint32_t displayNowMs =
        providers.readDisplayNowMs ? providers.readDisplayNowMs(providers.displayNowContext) : ctx.nowMs;

    ParsedFrameSignal parsedSignal;
    if (providers.collectParsedSignal) {
        parsedSignal = providers.collectParsedSignal(providers.parsedSignalContext);
    }

    // Consume the event edge so the bounded bus cannot fill while the warning
    // owns presentation, but do not run normal frames or blink refreshes.
    if (ctx.presentationSuppressed) {
        return;
    }

    DisplayOrchestrationParsedResult parsedResult;
    DisplayOrchestrationParsedContext parsedCtx;
    parsedCtx.nowMs = displayNowMs;
    parsedCtx.parsedReady = parsedSignal.parsedReady;
    parsedCtx.bootSplashHoldActive = ctx.bootSplashHoldActive;

    if (providers.runParsedFrame) {
        parsedResult = providers.runParsedFrame(providers.parsedFrameContext, parsedCtx);
    }

    bool pipelineRanThisLoop = false;
    if (parsedResult.runDisplayPipeline) {
        if (providers.runDisplayPipeline) {
            providers.runDisplayPipeline(providers.displayPipelineContext, displayNowMs);
        }
        pipelineRanThisLoop = true;
    }

    if (providers.runLightweightRefresh) {
        DisplayOrchestrationRefreshContext refreshCtx;
        refreshCtx.nowMs = displayNowMs;
        refreshCtx.bootSplashHoldActive = ctx.bootSplashHoldActive;
        refreshCtx.overloadLateThisLoop = ctx.overloadLateThisLoop;
        refreshCtx.pipelineRanThisLoop = pipelineRanThisLoop;
        const DisplayOrchestrationRefreshResult refreshResult =
            providers.runLightweightRefresh(providers.lightweightRefreshContext, refreshCtx);
        if (refreshResult.runBlinkRefresh && providers.runBlinkRefresh) {
            providers.runBlinkRefresh(providers.blinkRefreshContext, displayNowMs);
        }
    }
}
