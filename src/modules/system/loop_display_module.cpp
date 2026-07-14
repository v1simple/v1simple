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
        if (providers.recordNotifyToDisplayMs && parsedSignal.parsedTsMs != 0 &&
            displayNowMs >= parsedSignal.parsedTsMs) {
            providers.recordNotifyToDisplayMs(providers.notifyPerfContext, displayNowMs - parsedSignal.parsedTsMs);
        }

        if (providers.runDisplayPipeline) {
            if (providers.timestampUs && providers.recordDispPipeUs) {
                const uint32_t startUs = providers.timestampUs(providers.timestampContext);
                providers.runDisplayPipeline(providers.displayPipelineContext, displayNowMs);
                providers.recordDispPipeUs(providers.dispPipePerfContext,
                                           providers.timestampUs(providers.timestampContext) - startUs);
            } else {
                providers.runDisplayPipeline(providers.displayPipelineContext, displayNowMs);
            }
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
            if (providers.timestampUs && providers.recordDispPipeUs) {
                const uint32_t startUs = providers.timestampUs(providers.timestampContext);
                providers.runBlinkRefresh(providers.blinkRefreshContext, displayNowMs);
                providers.recordDispPipeUs(providers.dispPipePerfContext,
                                           providers.timestampUs(providers.timestampContext) - startUs);
            } else {
                providers.runBlinkRefresh(providers.blinkRefreshContext, displayNowMs);
            }
        }
    }
}
