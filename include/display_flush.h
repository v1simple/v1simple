// display_flush.h — Shared DISPLAY_FLUSH macro for display_*.cpp files
// Wraps tft_->flush() with perf instrumentation on Arduino_GFX builds.
#pragma once

#include "perf_metrics.h"

#define DISPLAY_FLUSH() do { \
    if (tft_) { \
        uint32_t _start = PERF_TIMESTAMP_US(); \
        uint32_t _areaPx = static_cast<uint32_t>(tft_->width()) * static_cast<uint32_t>(tft_->height()); \
        tft_->flush(); \
        perfRecordFlushUs(PERF_TIMESTAMP_US() - _start, _areaPx, true); \
    } \
} while(0)

