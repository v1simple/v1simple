#include "display_frequency_raster_cache.h"

#include <Arduino.h>
#include <algorithm>
#include <cstring>
#include <esp_heap_caps.h>

namespace {
constexpr uint32_t kFrequencyRasterPsramCaps = MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM;
}

bool DisplayFrequencyRasterCache::begin(uint16_t maxLogicalW,
                                        uint16_t maxLogicalH,
                                        uint8_t capacity) {
    release();

    if (maxLogicalW == 0 || maxLogicalH == 0 || capacity == 0) {
        return false;
    }

    capacity_ = std::min<uint8_t>(capacity, kMaxEntries);
    maxLogicalW_ = maxLogicalW;
    maxLogicalH_ = maxLogicalH;
    entryPixels_ = static_cast<uint32_t>(maxLogicalW_) * static_cast<uint32_t>(maxLogicalH_);
    bytes_ = static_cast<size_t>(entryPixels_) * sizeof(uint16_t) * capacity_;

    pool_ = static_cast<uint16_t*>(heap_caps_malloc(bytes_, kFrequencyRasterPsramCaps));
    if (!pool_) {
        Serial.printf("[DisplayCache] frequency raster cache disabled: alloc %u bytes failed\n",
                      static_cast<unsigned>(bytes_));
        capacity_ = 0;
        bytes_ = 0;
        entryPixels_ = 0;
        maxLogicalW_ = 0;
        maxLogicalH_ = 0;
        return false;
    }

    invalidate();
    Serial.printf("[DisplayCache] frequency raster cache: entries=%u bytes=%u\n",
                  static_cast<unsigned>(capacity_),
                  static_cast<unsigned>(bytes_));
    return true;
}

void DisplayFrequencyRasterCache::release() {
    if (pool_) {
        heap_caps_free(pool_);
    }
    pool_ = nullptr;
    capacity_ = 0;
    bytes_ = 0;
    entryPixels_ = 0;
    maxLogicalW_ = 0;
    maxLogicalH_ = 0;
    useCounter_ = 0;
    hitCount_ = 0;
    missCount_ = 0;
    storeCount_ = 0;
    std::fill(std::begin(entries_), std::end(entries_), Entry{});
}

void DisplayFrequencyRasterCache::invalidate() {
    for (Entry& entry : entries_) {
        entry.valid = false;
        entry.lastUsed = 0;
    }
    useCounter_ = 0;
}

bool DisplayFrequencyRasterCache::restore(const FrequencyRasterKey& key,
                                          uint16_t* framebuffer,
                                          int16_t rawStride) {
    if (!enabled() || !framebuffer) {
        return false;
    }

    const int slot = findEntry(key);
    if (slot < 0) {
        ++missCount_;
        return false;
    }

    int16_t physX = 0;
    int16_t physY = 0;
    int16_t physW = 0;
    int16_t physH = 0;
    if (!copyGeometry(key, rawStride, physX, physY, physW, physH)) {
        return false;
    }

    uint16_t* src = pixelsFor(static_cast<uint8_t>(slot));
    for (int16_t row = 0; row < physH; ++row) {
        uint16_t* dstRow = framebuffer +
                           static_cast<uint32_t>(physY + row) * static_cast<uint32_t>(rawStride) +
                           static_cast<uint32_t>(physX);
        const uint16_t* srcRow = src + static_cast<uint32_t>(row) * maxLogicalH_;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(physW) * sizeof(uint16_t));
    }

    entries_[slot].lastUsed = ++useCounter_;
    ++hitCount_;
    return true;
}

bool DisplayFrequencyRasterCache::store(const FrequencyRasterKey& key,
                                        const uint16_t* framebuffer,
                                        int16_t rawStride) {
    if (!enabled() || !framebuffer) {
        return false;
    }

    int16_t physX = 0;
    int16_t physY = 0;
    int16_t physW = 0;
    int16_t physH = 0;
    if (!copyGeometry(key, rawStride, physX, physY, physW, physH)) {
        return false;
    }

    const int slot = chooseStoreSlot();
    uint16_t* dst = pixelsFor(static_cast<uint8_t>(slot));
    for (int16_t row = 0; row < physH; ++row) {
        const uint16_t* srcRow = framebuffer +
                                 static_cast<uint32_t>(physY + row) * static_cast<uint32_t>(rawStride) +
                                 static_cast<uint32_t>(physX);
        uint16_t* dstRow = dst + static_cast<uint32_t>(row) * maxLogicalH_;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(physW) * sizeof(uint16_t));
    }

    entries_[slot].valid = true;
    entries_[slot].key = key;
    entries_[slot].lastUsed = ++useCounter_;
    ++storeCount_;
    return true;
}

bool DisplayFrequencyRasterCache::keyEquals(const FrequencyRasterKey& a,
                                            const FrequencyRasterKey& b) {
    return a.color == b.color &&
           a.bg == b.bg &&
           a.fontSize == b.fontSize &&
           a.paletteRevision == b.paletteRevision &&
           a.x == b.x &&
           a.y == b.y &&
           a.w == b.w &&
           a.h == b.h &&
           a.flags == b.flags &&
           std::strncmp(a.text, b.text, sizeof(a.text)) == 0;
}

int DisplayFrequencyRasterCache::findEntry(const FrequencyRasterKey& key) const {
    for (uint8_t i = 0; i < capacity_; ++i) {
        if (entries_[i].valid && keyEquals(entries_[i].key, key)) {
            return i;
        }
    }
    return -1;
}

int DisplayFrequencyRasterCache::chooseStoreSlot() {
    for (uint8_t i = 0; i < capacity_; ++i) {
        if (!entries_[i].valid) {
            return i;
        }
    }

    uint8_t oldestSlot = 0;
    uint32_t oldestUse = entries_[0].lastUsed;
    for (uint8_t i = 1; i < capacity_; ++i) {
        if (entries_[i].lastUsed < oldestUse) {
            oldestUse = entries_[i].lastUsed;
            oldestSlot = i;
        }
    }
    return oldestSlot;
}

bool DisplayFrequencyRasterCache::copyGeometry(const FrequencyRasterKey& key,
                                               int16_t rawStride,
                                               int16_t& physX,
                                               int16_t& physY,
                                               int16_t& physW,
                                               int16_t& physH) const {
    if (key.w <= 0 || key.h <= 0 ||
        key.w > static_cast<int16_t>(maxLogicalW_) ||
        key.h > static_cast<int16_t>(maxLogicalH_) ||
        rawStride <= 0) {
        return false;
    }

    // Arduino_Canvas is created as native 172x640 with rotation=1.
    // Logical(lx, ly) maps to physical(px = rawStride - 1 - ly, py = lx).
    physX = static_cast<int16_t>(rawStride - key.y - key.h);
    physY = key.x;
    physW = key.h;
    physH = key.w;

    if (physX < 0 ||
        physY < 0 ||
        physX + physW > rawStride ||
        physW > static_cast<int16_t>(maxLogicalH_)) {
        return false;
    }
    return true;
}

uint16_t* DisplayFrequencyRasterCache::pixelsFor(uint8_t slot) const {
    return pool_ + static_cast<uint32_t>(slot) * entryPixels_;
}
