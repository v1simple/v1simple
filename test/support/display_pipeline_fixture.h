#pragma once

// Display-only fixture entrypoint: preparation and painting cannot admit voice.
inline void presentParsedFrame(DisplayPipelineModule& pipeline, uint32_t nowMs) {
    RenderFrame frame;
    if (pipeline.prepareParsed(nowMs, frame)) pipeline.presentPrepared(frame, nowMs);
}
