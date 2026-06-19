#pragma once

#include <cstdint>

namespace DisplayBleFreshness {

static constexpr uint32_t STALE_AFTER_MS = 1000;

inline bool isFresh(uint32_t updatedAtMs,
                    uint32_t nowMs,
                    uint32_t staleAfterMs = STALE_AFTER_MS) {
    return updatedAtMs != 0 &&
           static_cast<uint32_t>(nowMs - updatedAtMs) <= staleAfterMs;
}

}  // namespace DisplayBleFreshness
