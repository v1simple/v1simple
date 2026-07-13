#include "display_frequency_digit_atlas.h"

#include <Arduino.h>
#include <algorithm>
#include <cstring>
#include <esp_heap_caps.h>

namespace {
constexpr uint32_t kDigitAtlasPsramCaps = MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM;

uint8_t scale5To8(uint16_t v) {
    return static_cast<uint8_t>((v * 255u + 15u) / 31u);
}

uint8_t scale6To8(uint16_t v) {
    return static_cast<uint8_t>((v * 255u + 31u) / 63u);
}

uint16_t scale8To5(uint16_t v) {
    return static_cast<uint16_t>((v * 31u + 127u) / 255u);
}

uint16_t scale8To6(uint16_t v) {
    return static_cast<uint16_t>((v * 63u + 127u) / 255u);
}
}  // namespace

bool DisplayFrequencyDigitAtlas::begin(uint16_t maxLogicalW, uint16_t maxLogicalH) {
    release();

    if (maxLogicalW == 0 || maxLogicalH == 0) {
        return false;
    }

    maxLogicalW_ = maxLogicalW;
    maxLogicalH_ = maxLogicalH;
    slotBytes_ = static_cast<uint32_t>(maxLogicalW_) * static_cast<uint32_t>(maxLogicalH_);
    bytes_ = static_cast<size_t>(slotBytes_) * kSlotCount;

    pool_ = static_cast<uint8_t*>(heap_caps_malloc(bytes_, kDigitAtlasPsramCaps));
    if (!pool_) {
        Serial.printf("[DisplayCache] frequency digit atlas disabled: alloc %u bytes failed\n",
                      static_cast<unsigned>(bytes_));
        maxLogicalW_ = 0;
        maxLogicalH_ = 0;
        slotBytes_ = 0;
        bytes_ = 0;
        return false;
    }

    invalidate();
    Serial.printf("[DisplayCache] frequency digit atlas: slots=%u bytes=%u\n",
                  static_cast<unsigned>(kSlotCount),
                  static_cast<unsigned>(bytes_));
    return true;
}

void DisplayFrequencyDigitAtlas::release() {
    if (pool_) {
        heap_caps_free(pool_);
    }
    pool_ = nullptr;
    maxLogicalW_ = 0;
    maxLogicalH_ = 0;
    slotBytes_ = 0;
    bytes_ = 0;
    storeCount_ = 0;
    hitCount_ = 0;
    missCount_ = 0;
    blendLutBuildCount_ = 0;
    validCellCount_ = 0;
    ready_ = false;
    blendLutValid_ = false;
    for (Cell& cell : cells_) {
        cell = Cell{};
    }
}

void DisplayFrequencyDigitAtlas::invalidate() {
    for (Cell& cell : cells_) {
        cell = Cell{};
    }
    validCellCount_ = 0;
    ready_ = false;
    storeCount_ = 0;
    hitCount_ = 0;
    missCount_ = 0;
    if (pool_) {
        std::memset(pool_, 0, bytes_);
    }
}

bool DisplayFrequencyDigitAtlas::ready() const {
    return enabled() && ready_;
}

bool DisplayFrequencyDigitAtlas::isNumericFrequencyText(const char* text) {
    if (!text) {
        return false;
    }
    for (uint8_t i = 0; i < kTextPositions; ++i) {
        const char c = text[i];
        if (i == 2) {
            if (c != '.') {
                return false;
            }
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return text[kTextPositions] == '\0';
}

bool DisplayFrequencyDigitAtlas::storeCell(uint8_t position,
                                           char symbol,
                                           int16_t x,
                                           int16_t y,
                                           int16_t w,
                                           int16_t h,
                                           const uint16_t* framebuffer,
                                           int16_t rawStride,
                                           uint16_t bg) {
    if (!enabled() || !framebuffer) {
        return false;
    }

    const int slot = slotFor(position, symbol);
    if (slot < 0) {
        return false;
    }

    int16_t physX = 0;
    int16_t physY = 0;
    int16_t physW = 0;
    int16_t physH = 0;
    if (!copyGeometry(x, y, w, h, rawStride, physX, physY, physW, physH)) {
        return false;
    }

    uint8_t* dst = alphaFor(static_cast<uint8_t>(slot));
    std::memset(dst, 0, slotBytes_);
    for (int16_t row = 0; row < physH; ++row) {
        const uint16_t* srcRow = framebuffer +
                                 static_cast<uint32_t>(physY + row) * static_cast<uint32_t>(rawStride) +
                                 static_cast<uint32_t>(physX);
        uint8_t* dstRow = dst + static_cast<uint32_t>(row) * maxLogicalH_;
        for (int16_t col = 0; col < physW; ++col) {
            dstRow[col] = alphaFromRgb565(srcRow[col], bg);
        }
    }

    if (!cells_[slot].valid) {
        ++validCellCount_;
    }
    cells_[slot].valid = true;
    cells_[slot].x = x;
    cells_[slot].y = y;
    cells_[slot].w = w;
    cells_[slot].h = h;
    ready_ = (validCellCount_ == kSlotCount);
    ++storeCount_;
    return true;
}

bool DisplayFrequencyDigitAtlas::restoreText(const char* text,
                                             uint16_t color,
                                             uint16_t bg,
                                             uint16_t* framebuffer,
                                             int16_t rawStride) {
    if (!ready() || !framebuffer || !isNumericFrequencyText(text)) {
        ++missCount_;
        return false;
    }

    uint8_t slots[kTextPositions] = {};
    for (uint8_t pos = 0; pos < kTextPositions; ++pos) {
        const int slot = slotFor(pos, text[pos]);
        if (slot < 0 || !cells_[slot].valid) {
            ++missCount_;
            return false;
        }
        slots[pos] = static_cast<uint8_t>(slot);
    }

    const uint16_t* blendLut = blendLutFor(color, bg);
    for (uint8_t pos = 0; pos < kTextPositions; ++pos) {
        const uint8_t slot = slots[pos];
        const Cell& cell = cells_[slot];

        int16_t physX = 0;
        int16_t physY = 0;
        int16_t physW = 0;
        int16_t physH = 0;
        if (!copyGeometry(cell.x, cell.y, cell.w, cell.h, rawStride, physX, physY, physW, physH)) {
            ++missCount_;
            return false;
        }

        const uint8_t* src = alphaFor(static_cast<uint8_t>(slot));
        for (int16_t row = 0; row < physH; ++row) {
            uint16_t* dstRow = framebuffer +
                               static_cast<uint32_t>(physY + row) * static_cast<uint32_t>(rawStride) +
                               static_cast<uint32_t>(physX);
            const uint8_t* srcRow = src + static_cast<uint32_t>(row) * maxLogicalH_;
            for (int16_t col = 0; col < physW; ++col) {
                const uint8_t alpha = srcRow[col];
                if (alpha != 0) {
                    dstRow[col] = blendLut[alpha];
                }
            }
        }
    }

    ++hitCount_;
    return true;
}

bool DisplayFrequencyDigitAtlas::changedTextRect(const char* previousText,
                                                 const char* nextText,
                                                 int16_t& x,
                                                 int16_t& y,
                                                 int16_t& w,
                                                 int16_t& h) const {
    x = 0;
    y = 0;
    w = 0;
    h = 0;

    if (!ready() ||
        !isNumericFrequencyText(previousText) ||
        !isNumericFrequencyText(nextText)) {
        return false;
    }

    bool any = false;
    int32_t minX = 0;
    int32_t minY = 0;
    int32_t maxX = 0;
    int32_t maxY = 0;

    auto includeCell = [&](const Cell& cell) {
        if (!cell.valid || cell.w <= 0 || cell.h <= 0) {
            return false;
        }
        const int32_t cellMinX = cell.x;
        const int32_t cellMinY = cell.y;
        const int32_t cellMaxX = static_cast<int32_t>(cell.x) + cell.w;
        const int32_t cellMaxY = static_cast<int32_t>(cell.y) + cell.h;
        if (!any) {
            minX = cellMinX;
            minY = cellMinY;
            maxX = cellMaxX;
            maxY = cellMaxY;
            any = true;
        } else {
            minX = std::min(minX, cellMinX);
            minY = std::min(minY, cellMinY);
            maxX = std::max(maxX, cellMaxX);
            maxY = std::max(maxY, cellMaxY);
        }
        return true;
    };

    for (uint8_t pos = 0; pos < kTextPositions; ++pos) {
        if (previousText[pos] == nextText[pos]) {
            continue;
        }

        const int previousSlot = slotFor(pos, previousText[pos]);
        const int nextSlot = slotFor(pos, nextText[pos]);
        if (previousSlot < 0 ||
            nextSlot < 0 ||
            !includeCell(cells_[previousSlot]) ||
            !includeCell(cells_[nextSlot])) {
            return false;
        }
    }

    if (!any || maxX <= minX || maxY <= minY) {
        return false;
    }

    x = static_cast<int16_t>(minX);
    y = static_cast<int16_t>(minY);
    w = static_cast<int16_t>(maxX - minX);
    h = static_cast<int16_t>(maxY - minY);
    return true;
}

bool DisplayFrequencyDigitAtlas::restoreTextInRect(const char* text,
                                                   uint16_t color,
                                                   uint16_t bg,
                                                   uint16_t* framebuffer,
                                                   int16_t rawStride,
                                                   int16_t rectX,
                                                   int16_t rectY,
                                                   int16_t rectW,
                                                   int16_t rectH) {
    if (!ready() ||
        !framebuffer ||
        !isNumericFrequencyText(text) ||
        rectW <= 0 ||
        rectH <= 0) {
        ++missCount_;
        return false;
    }

    uint8_t slots[kTextPositions] = {};
    for (uint8_t pos = 0; pos < kTextPositions; ++pos) {
        const int slot = slotFor(pos, text[pos]);
        if (slot < 0 || !cells_[slot].valid) {
            ++missCount_;
            return false;
        }
        slots[pos] = static_cast<uint8_t>(slot);
    }

    const uint16_t* blendLut = blendLutFor(color, bg);
    bool restoredAny = false;
    for (uint8_t pos = 0; pos < kTextPositions; ++pos) {
        const uint8_t slot = slots[pos];
        const Cell& cell = cells_[slot];
        if (!rectsIntersect(cell, rectX, rectY, rectW, rectH)) {
            continue;
        }

        int16_t physX = 0;
        int16_t physY = 0;
        int16_t physW = 0;
        int16_t physH = 0;
        if (!copyGeometry(cell.x, cell.y, cell.w, cell.h, rawStride, physX, physY, physW, physH)) {
            ++missCount_;
            return false;
        }

        const uint8_t* src = alphaFor(slot);
        for (int16_t row = 0; row < physH; ++row) {
            uint16_t* dstRow = framebuffer +
                               static_cast<uint32_t>(physY + row) * static_cast<uint32_t>(rawStride) +
                               static_cast<uint32_t>(physX);
            const uint8_t* srcRow = src + static_cast<uint32_t>(row) * maxLogicalH_;
            for (int16_t col = 0; col < physW; ++col) {
                const uint8_t alpha = srcRow[col];
                if (alpha != 0) {
                    dstRow[col] = blendLut[alpha];
                }
            }
        }
        restoredAny = true;
    }

    if (!restoredAny) {
        ++missCount_;
        return false;
    }

    ++hitCount_;
    return true;
}

int DisplayFrequencyDigitAtlas::slotFor(uint8_t position, char symbol) {
    if (position >= kTextPositions) {
        return -1;
    }
    if (position == 2) {
        return symbol == '.' ? kDotSlot : -1;
    }
    if (symbol < '0' || symbol > '9') {
        return -1;
    }

    const uint8_t densePos = position < 2 ? position : static_cast<uint8_t>(position - 1);
    return static_cast<int>(densePos) * kDigits + (symbol - '0');
}

bool DisplayFrequencyDigitAtlas::copyGeometry(int16_t x,
                                              int16_t y,
                                              int16_t w,
                                              int16_t h,
                                              int16_t rawStride,
                                              int16_t& physX,
                                              int16_t& physY,
                                              int16_t& physW,
                                              int16_t& physH) const {
    if (w <= 0 || h <= 0 ||
        w > static_cast<int16_t>(maxLogicalW_) ||
        h > static_cast<int16_t>(maxLogicalH_) ||
        rawStride <= 0) {
        return false;
    }

    // Arduino_Canvas is created as native 172x640 with rotation=1.
    physX = static_cast<int16_t>(rawStride - y - h);
    physY = x;
    physW = h;
    physH = w;

    if (physX < 0 ||
        physY < 0 ||
        physX + physW > rawStride ||
        physW > static_cast<int16_t>(maxLogicalH_) ||
        physH > static_cast<int16_t>(maxLogicalW_)) {
        return false;
    }
    return true;
}

uint8_t* DisplayFrequencyDigitAtlas::alphaFor(uint8_t slot) const {
    return pool_ + static_cast<uint32_t>(slot) * slotBytes_;
}

const uint16_t* DisplayFrequencyDigitAtlas::blendLutFor(uint16_t fg, uint16_t bg) {
    if (blendLutValid_ && blendLutFg_ == fg && blendLutBg_ == bg) {
        return blendLut_;
    }

    blendLut_[0] = bg;
    for (uint16_t alpha = 1; alpha < 256; ++alpha) {
        blendLut_[alpha] = blend565(fg, bg, static_cast<uint8_t>(alpha));
    }
    blendLutFg_ = fg;
    blendLutBg_ = bg;
    blendLutValid_ = true;
    ++blendLutBuildCount_;
    return blendLut_;
}

bool DisplayFrequencyDigitAtlas::rectsIntersect(const Cell& cell,
                                                int16_t x,
                                                int16_t y,
                                                int16_t w,
                                                int16_t h) {
    if (!cell.valid || cell.w <= 0 || cell.h <= 0 || w <= 0 || h <= 0) {
        return false;
    }

    const int32_t ax1 = cell.x;
    const int32_t ay1 = cell.y;
    const int32_t ax2 = static_cast<int32_t>(cell.x) + cell.w;
    const int32_t ay2 = static_cast<int32_t>(cell.y) + cell.h;
    const int32_t bx1 = x;
    const int32_t by1 = y;
    const int32_t bx2 = static_cast<int32_t>(x) + w;
    const int32_t by2 = static_cast<int32_t>(y) + h;
    return ax1 < bx2 && bx1 < ax2 && ay1 < by2 && by1 < ay2;
}

uint8_t DisplayFrequencyDigitAtlas::alphaFromRgb565(uint16_t px, uint16_t bg) {
    if (px == bg) {
        return 0;
    }

    const uint8_t r = scale5To8((px >> 11) & 0x1F);
    const uint8_t g = scale6To8((px >> 5) & 0x3F);
    const uint8_t b = scale5To8(px & 0x1F);
    const uint8_t br = scale5To8((bg >> 11) & 0x1F);
    const uint8_t bgc = scale6To8((bg >> 5) & 0x3F);
    const uint8_t bb = scale5To8(bg & 0x1F);

    const int dr = static_cast<int>(r) - static_cast<int>(br);
    const int dg = static_cast<int>(g) - static_cast<int>(bgc);
    const int db = static_cast<int>(b) - static_cast<int>(bb);
    const int alpha = std::max(dr, std::max(dg, db));
    if (alpha <= 0) {
        return 255;
    }
    return static_cast<uint8_t>(alpha > 255 ? 255 : alpha);
}

uint16_t DisplayFrequencyDigitAtlas::blend565(uint16_t fg, uint16_t bg, uint8_t alpha) {
    if (alpha == 255) {
        return fg;
    }

    const uint16_t inv = static_cast<uint16_t>(255u - alpha);
    const uint16_t fr = scale5To8((fg >> 11) & 0x1F);
    const uint16_t fgG = scale6To8((fg >> 5) & 0x3F);
    const uint16_t fb = scale5To8(fg & 0x1F);
    const uint16_t br = scale5To8((bg >> 11) & 0x1F);
    const uint16_t bgG = scale6To8((bg >> 5) & 0x3F);
    const uint16_t bb = scale5To8(bg & 0x1F);

    const uint16_t r = static_cast<uint16_t>((fr * alpha + br * inv + 127u) / 255u);
    const uint16_t g = static_cast<uint16_t>((fgG * alpha + bgG * inv + 127u) / 255u);
    const uint16_t b = static_cast<uint16_t>((fb * alpha + bb * inv + 127u) / 255u);
    return static_cast<uint16_t>((scale8To5(r) << 11) | (scale8To6(g) << 5) | scale8To5(b));
}
