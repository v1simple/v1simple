#pragma once

class DisplayPreviewModule;
class DisplayPipelineModule;

// Restores presentation after preview; the pipeline selects the current owner.
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
