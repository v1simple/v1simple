#pragma once

// Per-frame painted bounds. Live, resting and persisted frames full-flush when
// anything changed; external draws remain pending until the next frame consumes them.

#include <cstdint>

struct DrawnRegion {
    int16_t minX = 0;
    int16_t minY = 0;
    int16_t maxX = 0;
    int16_t maxY = 0;
    bool any = false;

    void reset() {
        any = false;
        minX = minY = maxX = maxY = 0;
    }

    void add(int16_t x, int16_t y, int16_t w, int16_t h) {
        if (w <= 0 || h <= 0)
            return;
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
};
