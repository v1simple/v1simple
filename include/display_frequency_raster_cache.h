/**
 * PSRAM-backed frequency raster cache.
 *
 * This cache stores already-rendered frequency-zone pixels in the same raw
 * framebuffer orientation used by Arduino_Canvas. It never talks to the panel;
 * callers copy cache hits back into the canvas and the existing display flush
 * policy remains authoritative.
 */

#pragma once

#include <cstddef>
#include <cstdint>

struct FrequencyRasterKey {
    char text[16] = {0};
    uint16_t color = 0;
    uint16_t bg = 0;
    uint16_t fontSize = 0;
    uint32_t paletteRevision = 0;
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;
    uint8_t flags = 0;
};

class DisplayFrequencyRasterCache {
public:
    static constexpr uint8_t kMaxEntries = 8;

    DisplayFrequencyRasterCache() = default;
    ~DisplayFrequencyRasterCache() = default;

    DisplayFrequencyRasterCache(const DisplayFrequencyRasterCache&) = delete;
    DisplayFrequencyRasterCache& operator=(const DisplayFrequencyRasterCache&) = delete;

    bool begin(uint16_t maxLogicalW, uint16_t maxLogicalH, uint8_t capacity = kMaxEntries);
    void release();
    void invalidate();

    bool enabled() const { return pool_ != nullptr && capacity_ > 0; }
    uint8_t capacity() const { return capacity_; }
    size_t bytes() const { return bytes_; }
    uint32_t hitCount() const { return hitCount_; }
    uint32_t missCount() const { return missCount_; }
    uint32_t storeCount() const { return storeCount_; }

    bool restore(const FrequencyRasterKey& key, uint16_t* framebuffer, int16_t rawStride);
    bool store(const FrequencyRasterKey& key, const uint16_t* framebuffer, int16_t rawStride);

private:
    struct Entry {
        bool valid = false;
        FrequencyRasterKey key{};
        uint32_t lastUsed = 0;
    };

    static bool keyEquals(const FrequencyRasterKey& a, const FrequencyRasterKey& b);
    int findEntry(const FrequencyRasterKey& key) const;
    int chooseStoreSlot();
    bool copyGeometry(const FrequencyRasterKey& key,
                      int16_t rawStride,
                      int16_t& physX,
                      int16_t& physY,
                      int16_t& physW,
                      int16_t& physH) const;
    uint16_t* pixelsFor(uint8_t slot) const;

    Entry entries_[kMaxEntries]{};
    uint16_t* pool_ = nullptr;
    uint16_t maxLogicalW_ = 0;
    uint16_t maxLogicalH_ = 0;
    uint32_t entryPixels_ = 0;
    size_t bytes_ = 0;
    uint8_t capacity_ = 0;
    uint32_t useCounter_ = 0;
    uint32_t hitCount_ = 0;
    uint32_t missCount_ = 0;
    uint32_t storeCount_ = 0;
};
