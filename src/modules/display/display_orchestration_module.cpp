#include "display_orchestration_module.h"

#include "modules/quiet/quiet_coordinator_module.h"

#ifndef UNIT_TEST
#include "ble_client.h"
#include "display.h"
#include "modules/ble/ble_queue_module.h"
#include "modules/display/display_preview_module.h"
#include "modules/display/display_restore_module.h"
#include "modules/speed_mute/speed_mute_module.h"
#include "modules/volume_fade/volume_fade_module.h"
#include "packet_parser.h"
#include "perf_metrics.h"
#include "settings.h"
#endif

#include "modules/quiet/quiet_coordinator_templates.h"

void DisplayOrchestrationModule::begin(V1Display* displayPtr,
                                       V1BLEClient* bleClient,
                                       BleQueueModule* bleQueueModule,
                                       DisplayPreviewModule* previewModule,
                                       DisplayRestoreModule* restoreModule,
                                       PacketParser* parserPtr,
                                       SettingsManager* settings,
                                       VolumeFadeModule* volumeFadeModule,
                                       SpeedMuteModule* speedMuteModule,
                                       QuietCoordinatorModule* quietCoordinator) {
    display_ = displayPtr;
    ble_ = bleClient;
    bleQueue_ = bleQueueModule;
    preview_ = previewModule;
    restore_ = restoreModule;
    parser_ = parserPtr;
    settings_ = settings;
    volumeFade_ = volumeFadeModule;
    speedMute_ = speedMuteModule;
    quiet_ = quietCoordinator;
}

void DisplayOrchestrationModule::syncQuietPresentation() {
    if (!display_ || !quiet_) {
        return;
    }

    const QuietPresentationState& presentation = quiet_->getPresentationState();
    display_->setSpeedVolZeroActive(presentation.speedVolZeroActive);
}

bool DisplayOrchestrationModule::processSpeedVolume(const uint32_t nowMs) {
    if (!quiet_ || !speedMute_) {
        return false;
    }
    return quiet_->processSpeedVolume(nowMs, *speedMute_, volumeFade_);
}

void DisplayOrchestrationModule::executeVolumeFade(const uint32_t nowMs) {
    if (quiet_) {
        (void)quiet_->executeVolumeFade(nowMs, volumeFade_);
    }
}

void DisplayOrchestrationModule::processEarly(const DisplayOrchestrationEarlyContext& ctx) {
    if (!display_ || !preview_ || !restore_) {
        return;
    }

    const bool previewRunning = preview_->isRunning();

    if (!ctx.overloadThisLoop && !ctx.bootSplashHoldActive) {
        display_->setBleContext(ctx.bleContext);
        if (!previewRunning) {
            display_->setBLEProxyStatus(ctx.bleContext.v1Connected,
                                       ctx.bleContext.proxyConnected,
                                       ctx.bleReceiving);
        }
    }

    if (!ctx.overloadThisLoop && !ctx.bootSplashHoldActive) {
        if (previewRunning) {
            preview_->update();
        } else {
            restore_->process();
        }
    }
}

DisplayOrchestrationParsedResult DisplayOrchestrationModule::processParsedFrame(
        const DisplayOrchestrationParsedContext& ctx) {
    DisplayOrchestrationParsedResult result;
    if (!display_ || !ble_ || !bleQueue_ || !preview_ || !parser_ || !settings_) {
        return result;
    }

    if (!ctx.parsedReady) {
        syncQuietPresentation();
        result.reasonSkipped = "no_parsed_frame";
        return result;
    }

    if (ctx.bootSplashHoldActive) {
        syncQuietPresentation();
        result.reasonSkipped = "boot_splash";
        return result;
    }

    // Speed volume: lower/restore V1 volume based on speed mute state.
    // Gates volume fade.
    const bool speedVolBusy = processSpeedVolume(ctx.nowMs);
    syncQuietPresentation();

    if (preview_->isRunning()) {
        result.reasonSkipped = "preview_running";
        return result;
    }

    result.runDisplayPipeline = true;
    if (!speedVolBusy) {
        executeVolumeFade(ctx.nowMs);
    }
    return result;
}

DisplayOrchestrationRefreshResult DisplayOrchestrationModule::processLightweightRefresh(
        const DisplayOrchestrationRefreshContext& ctx) {
    DisplayOrchestrationRefreshResult result;
    if (!display_ || !ble_ || !preview_ || !parser_) {
        return result;
    }

    // Report whether a renderable priority alert exists.
    const bool loopHasAlerts = parser_->hasAlerts();
    AlertData loopPriority;
    const bool loopHasRenderablePriority =
        loopHasAlerts && parser_->getRenderablePriorityAlert(loopPriority);
    result.signalPriorityActive = loopHasRenderablePriority;

    // ── D2 fix: blink-refresh tick ────────────────────────────────────────
    // Without this path, V1Display::updateBlinkPhase_() only advances when a
    // parsed packet drives the pipeline. Field log arrow_diag01 measured
    // ~150 ms toggle deltas vs the 96 ms spec target — the renderer was
    // capped by V1's packet cadence rather than the blink interval. This
    // requests a best-effort owner refresh whenever live blink sources are
    // active AND no parsed frame ran this loop AND ≥96 ms have elapsed
    // since the renderer's own last toggle (so the refresh is guaranteed
    // to produce a useful phase flip — not eaten by an internal threshold
    // beat). Suppressed on boot splash, preview, late-overload loops, and
    // BLE disconnect to honor the priority stack.
    if (!ctx.pipelineRanThisLoop &&
        !ctx.bootSplashHoldActive &&
        !ctx.overloadLateThisLoop &&
        !preview_->isRunning() &&
        ble_->isConnected() &&
        loopHasRenderablePriority) {
        const DisplayState liveState = parser_->getDisplayState();
        const bool flashActive =
            (liveState.flashBits != 0) ||
            (liveState.bandFlashBits != 0) ||
            (parser_->getAlertCount() > 1 && liveState.priorityArrow != DIR_NONE) ||
            (liveState.bogeyCounterByte != liveState.bogeyCounterByte2);
        if (flashActive) {
            const uint32_t lastToggle =
                static_cast<uint32_t>(display_->getLastBlinkToggleMs());
            const uint32_t sinceToggle = ctx.nowMs - lastToggle;
            if (sinceToggle >= V1Display::getBlinkIntervalMs()) {
                result.runBlinkRefresh = true;
            }
        }
    }

    return result;
}
