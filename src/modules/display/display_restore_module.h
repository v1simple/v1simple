#pragma once

class DisplayPreviewModule;
class DisplayPipelineModule;

/**
 * DisplayRestoreModule - Handles display restoration after preview modes end.
 *
 * The display pipeline is the sole authority for selecting and restoring the
 * current presentation owner.
 */
class DisplayRestoreModule {
  public:
    void begin(DisplayPreviewModule& preview, DisplayPipelineModule& displayPipeline);

    /**
     * Check if preview ended and restore display if needed.
     * @return true if restoration occurred, false otherwise
     */
    bool process();

  private:
    DisplayPreviewModule* previewModule_ = nullptr;
    DisplayPipelineModule* displayPipelineModule_ = nullptr;
};
