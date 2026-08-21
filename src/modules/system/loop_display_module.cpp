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
        bool hasDisplayPipelineCompletedAtMs = false;
        uint32_t displayPipelineCompletedAtMs = 0;
        if (providers.runDisplayPipeline) {
            const bool measurePipelineDuration = providers.timestampUs && providers.recordDispPipeUs;
            const uint32_t pipelineStartedAtUs =
                measurePipelineDuration ? providers.timestampUs(providers.timestampContext) : 0;
            providers.runDisplayPipeline(providers.displayPipelineContext, displayNowMs);
            const uint32_t pipelineCompletedAtUs =
                measurePipelineDuration ? providers.timestampUs(providers.timestampContext) : 0;

            if (providers.readDisplayNowMs && providers.recordNotifyToDisplayPipelineCompleteMs &&
                parsedSignal.parsedTsMs != 0) {
                displayPipelineCompletedAtMs = providers.readDisplayNowMs(providers.displayNowContext);
                hasDisplayPipelineCompletedAtMs = true;
            }
            if (measurePipelineDuration) {
                providers.recordDispPipeUs(providers.dispPipePerfContext, pipelineCompletedAtUs - pipelineStartedAtUs);
            }
        }
        if (hasDisplayPipelineCompletedAtMs) {
            if (displayPipelineCompletedAtMs >= parsedSignal.parsedTsMs) {
                providers.recordNotifyToDisplayPipelineCompleteMs(providers.notifyPipelineCompletePerfContext,
                                                                  displayPipelineCompletedAtMs -
                                                                      parsedSignal.parsedTsMs);
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
