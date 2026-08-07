/**
 * PSRAM-backed positioned digit atlas for OFR frequency text.
 *
 * The full-string raster cache is exact but scales with every distinct
 * frequency. This atlas stores alpha masks for the fixed DD.DDD numeric
 * positions, so arbitrary numeric frequencies can be composed with a bounded
 * memory footprint while the existing flush policy remains authoritative.
 */

#pragma once

#include <cstddef>
#include <cstdint>

class DisplayFrequencyDigitAtlas {
  public:
    static constexpr uint8_t kTextPositions = 6; // DD.DDD
    static constexpr uint8_t kDigitPositions = 5;
    static constexpr uint8_t kDigits = 10;
    static constexpr uint8_t kDotSlot = kDigitPositions * kDigits;
    static constexpr uint8_t kSlotCount = kDotSlot + 1;

    DisplayFrequencyDigitAtlas() = default;
    ~DisplayFrequencyDigitAtlas() = default;

    DisplayFrequencyDigitAtlas(const DisplayFrequencyDigitAtlas&) = delete;
    DisplayFrequencyDigitAtlas& operator=(const DisplayFrequencyDigitAtlas&) = delete;

    bool begin(uint16_t maxLogicalW, uint16_t maxLogicalH);
    void release();
    void invalidate();

    bool enabled() const { return pool_ != nullptr; }
    bool ready() const;
    size_t bytes() const { return bytes_; }
    uint32_t storeCount() const { return storeCount_; }
    uint32_t hitCount() const { return hitCount_; }
    uint32_t missCount() const { return missCount_; }
    uint32_t blendLutBuildCount() const { return blendLutBuildCount_; }

    static bool isNumericFrequencyText(const char* text);

    bool storeCell(uint8_t position, char symbol, int16_t x, int16_t y, int16_t w, int16_t h,
                   const uint16_t* framebuffer, int16_t rawStride, uint16_t bg);

    bool restoreText(const char* text, uint16_t color, uint16_t bg, uint16_t* framebuffer, int16_t rawStride);

    bool changedTextRect(const char* previousText, const char* nextText, int16_t& x, int16_t& y, int16_t& w,
                         int16_t& h) const;

    bool restoreTextInRect(const char* text, uint16_t color, uint16_t bg, uint16_t* framebuffer, int16_t rawStride,
                           int16_t rectX, int16_t rectY, int16_t rectW, int16_t rectH);

  private:
    struct Cell {
        bool valid = false;
        int16_t x = 0;
        int16_t y = 0;
        int16_t w = 0;
        int16_t h = 0;
    };

    static int slotFor(uint8_t position, char symbol);
    bool copyGeometry(int16_t x, int16_t y, int16_t w, int16_t h, int16_t rawStride, int16_t& physX, int16_t& physY,
                      int16_t& physW, int16_t& physH) const;
    uint8_t* alphaFor(uint8_t slot) const;
    const uint16_t* blendLutFor(uint16_t fg, uint16_t bg);

    static bool rectsIntersect(const Cell& cell, int16_t x, int16_t y, int16_t w, int16_t h);
    static uint8_t alphaFromRgb565(uint16_t px, uint16_t bg);
    static uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t alpha);

    Cell cells_[kSlotCount]{};
    uint8_t* pool_ = nullptr;
    uint16_t maxLogicalW_ = 0;
    uint16_t maxLogicalH_ = 0;
    uint32_t slotBytes_ = 0;
    size_t bytes_ = 0;
    uint16_t blendLut_[256]{};
    uint16_t blendLutFg_ = 0;
    uint16_t blendLutBg_ = 0;
    uint16_t validCellCount_ = 0;
    uint32_t storeCount_ = 0;
    uint32_t hitCount_ = 0;
    uint32_t missCount_ = 0;
    uint32_t blendLutBuildCount_ = 0;
    bool ready_ = false;
    bool blendLutValid_ = false;
};
