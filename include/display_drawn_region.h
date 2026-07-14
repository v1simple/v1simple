#pragma once

// ============================================================================
// DrawnRegion — per-frame bounding-rect accumulator for partial-flush dispatch
//
// Used by V1Display update paths to decide whether a frame actually repainted
// pixels. Live alert frames choose between full DISPLAY_FLUSH, region
// flushRegion(), and no-op cache-hit-skip. Resting/persisted frames use the
// same signal only to suppress unchanged full-panel flushes; any painted pixel
// in those paths still takes the safe full DISPLAY_FLUSH route.
//
// See docs/plans/PARTIAL_FLUSH_REGION_UNION_20260422.md for the full design,
// the bounded-drift Valentine's Law argument, and the hardware-soak
// acceptance criteria.
//
// This is NOT a DisplayDirtyFlags member — it is write-only-per-frame
// instrumentation, reset at frame boundaries and consumed at the end.
// scripts/check_dirty_flag_discipline.py's scope is DisplayDirtyFlags and is
// unaffected by this struct.
// ============================================================================

#include <cstdint>

namespace DisplayDirtyRegionSource {
constexpr uint8_t Unknown = 0x01;
constexpr uint8_t Frequency = 0x02;
constexpr uint8_t Bands = 0x04;
constexpr uint8_t SignalBars = 0x08;
constexpr uint8_t Arrows = 0x10;
constexpr uint8_t Status = 0x20;
constexpr uint8_t Indicators = 0x40;
constexpr uint8_t External = 0x80;
} // namespace DisplayDirtyRegionSource

struct DrawnRegion {
    struct Rect {
        int16_t x = 0;
        int16_t y = 0;
        int16_t w = 0;
        int16_t h = 0;
        uint8_t sourceMask = DisplayDirtyRegionSource::Unknown;

        uint32_t areaPx() const {
            if (w <= 0 || h <= 0)
                return 0;
            return static_cast<uint32_t>(w) * static_cast<uint32_t>(h);
        }
    };

    static constexpr uint8_t MAX_RECTS = 16;

    int16_t minX = 0;
    int16_t minY = 0;
    int16_t maxX = 0;
    int16_t maxY = 0;
    bool any = false;
    Rect rects[MAX_RECTS]{};
    uint8_t count = 0;
    uint8_t sources = 0;
    bool overflow = false;

    void reset() {
        any = false;
        minX = minY = maxX = maxY = 0;
        count = 0;
        sources = 0;
        overflow = false;
    }

    void add(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t sourceMask = DisplayDirtyRegionSource::Unknown) {
        if (w <= 0 || h <= 0)
            return;
        const uint8_t effectiveSource = sourceMask ? sourceMask : DisplayDirtyRegionSource::Unknown;
        if (count < MAX_RECTS) {
            rects[count++] = Rect{x, y, w, h, effectiveSource};
        } else {
            overflow = true;
        }
        sources |= effectiveSource;
        const int16_t x1 = static_cast<int16_t>(x + w);
        const int16_t y1 = static_cast<int16_t>(y + h);
        if (!any) {
            minX = x;
            minY = y;
            maxX = x1;
            maxY = y1;
            any = true;
            return;
        }
        if (x < minX)
            minX = x;
        if (y < minY)
            minY = y;
        if (x1 > maxX)
            maxX = x1;
        if (y1 > maxY)
            maxY = y1;
    }

    bool empty() const { return !any; }
    int16_t x() const { return any ? minX : int16_t{0}; }
    int16_t y() const { return any ? minY : int16_t{0}; }
    int16_t w() const { return any ? static_cast<int16_t>(maxX - minX) : int16_t{0}; }
    int16_t h() const { return any ? static_cast<int16_t>(maxY - minY) : int16_t{0}; }
    uint8_t rectCount() const { return count; }
    uint8_t sourceMask() const { return sources; }
    bool overflowed() const { return overflow; }
    const Rect& rectAt(uint8_t index) const { return rects[index]; }
    uint32_t areaPx() const {
        if (!any)
            return 0;
        return static_cast<uint32_t>(maxX - minX) * static_cast<uint32_t>(maxY - minY);
    }
};
