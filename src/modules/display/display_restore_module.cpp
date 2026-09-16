#include "display_restore_module.h"

#include <Arduino.h>
#include "display_preview_module.h"
#include "display_pipeline_module.h"

void DisplayRestoreModule::begin(DisplayPreviewModule& preview, DisplayPipelineModule& displayPipeline) {
    previewModule_ = &preview;
    displayPipelineModule_ = &displayPipeline;
}

bool DisplayRestoreModule::process() {
    if (!previewModule_ || !displayPipelineModule_)
        return false;

    if (!previewModule_->consumeEnded()) {
        return false;
    }

    const bool restored = displayPipelineModule_->restoreCurrentOwner(millis());

    if (restored) {
        Serial.println("[Display] Color preview ended - restored display_");
    }

    return restored;
}
