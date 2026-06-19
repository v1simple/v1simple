#pragma once

#include <cstdint>

// Free-function API wrapping DisplayPreviewModule (defined in main.cpp).
// Used by wifi_runtimes.cpp to drive color preview flows.
void requestColorPreviewHold(uint32_t durationMs);
bool isDisplayPreviewRunning();
void cancelDisplayPreview();
