#include "display_restore_module.h"

#include "display_pipeline_module.h"
#include "perf_metrics.h"

void DisplayRestoreModule::begin(V1Display* disp, PacketParser* pktParser, V1BLEClient* ble,
                                 DisplayPreviewModule* preview, DisplayPipelineModule* displayPipeline) {
    display_ = disp;
    parser_ = pktParser;
    bleClient_ = ble;
    previewModule_ = preview;
    displayPipelineModule_ = displayPipeline;
}

bool DisplayRestoreModule::process() {
    if (!previewModule_)
        return false;

    bool previewEnded = previewModule_->consumeEnded();

    if (!previewEnded) {
        return false;
    }

    bool restored = false;

    if (displayPipelineModule_) {
        restored = displayPipelineModule_->restoreCurrentOwner(millis());
    }
    if (!restored && display_ && parser_ && bleClient_) {
        // Defensive fallback for tests or partial wiring; production should use the pipeline.
        display_->forceNextRedraw();
        perfSetDisplayRenderScenario(PerfDisplayRenderScenario::Restore);
        const unsigned long renderStartUs = micros();
        if (bleClient_->isConnected()) {
            display_->update(parser_->getDisplayState());
        } else {
            display_->showScanning();
        }
        perfRecordDisplayScenarioRenderUs(micros() - renderStartUs);
        perfClearDisplayRenderScenario();
        restored = true;
    }

    if (restored) {
        Serial.println("[Display] Color preview ended - restored display_");
    }

    return restored;
}
