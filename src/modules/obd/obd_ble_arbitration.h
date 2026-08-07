#pragma once

#include <cstdint>

enum class ObdBleArbitrationRequest : uint8_t {
    NONE = 0,
    HOLD_PROXY_FOR_AUTO_OBD = 1,
    PREEMPT_PROXY_FOR_MANUAL_SCAN = 2,
};
