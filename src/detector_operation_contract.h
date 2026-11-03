#pragma once

#include <cstdint>

// Numeric values are part of the durable detector-operation record.
enum class DetectorComponentOutcome : uint8_t {
    NotRequested = 0,
    Pending = 1,
    Unchanged = 2,
    Sent = 3,
    Verified = 4,
    Unsupported = 5,
    Invalid = 6,
    Blocked = 7,
    WriteFailed = 8,
    ReadFailed = 9,
    Mismatch = 10,
    Timeout = 11,
    Disconnected = 12,
    SessionChanged = 13,
    LoadFailed = 14,
};
