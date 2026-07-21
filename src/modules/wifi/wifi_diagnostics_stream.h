#pragma once

#include <cstddef>
#include <cstdint>

namespace WifiDiagnosticsStream {

enum class AttemptState : uint8_t {
    Progress,
    WouldBlock,
    Disconnected,
    Error,
};

struct AttemptResult {
    AttemptState state = AttemptState::Error;
    size_t bytes = 0;
};

struct Config {
    // Keep a stalled socket below the five-second task-watchdog deadline.
    uint32_t stallTimeoutMs = 2500;
    // Bound even a peer that makes pathologically slow incremental progress.
    uint32_t totalTimeoutMs = 180000;
    uint16_t retryDelayMs = 1;
};

// Writes one already-read file chunk without allowing socket backpressure to
// monopolize the maintenance task. All time comparisons are wrap-safe for
// intervals shorter than UINT32_MAX milliseconds.
template <typename AttemptFn, typename NowFn, typename WaitFn, typename FeedWatchdogFn>
bool writeAll(const uint8_t* data, size_t length, AttemptFn&& attempt, NowFn&& now, WaitFn&& wait,
              FeedWatchdogFn&& feedWatchdog, uint32_t operationStartedAtMs, const Config& config = Config()) {
    if ((!data && length > 0) || config.stallTimeoutMs == 0 || config.totalTimeoutMs == 0) {
        return false;
    }

    uint32_t lastProgressAtMs = static_cast<uint32_t>(now());
    size_t offset = 0;

    while (offset < length) {
        const uint32_t beforeAttemptMs = static_cast<uint32_t>(now());
        if (static_cast<uint32_t>(beforeAttemptMs - operationStartedAtMs) >= config.totalTimeoutMs) {
            return false;
        }

        feedWatchdog();
        const AttemptResult result = attempt(data + offset, length - offset);
        const uint32_t afterAttemptMs = static_cast<uint32_t>(now());
        if (static_cast<uint32_t>(afterAttemptMs - operationStartedAtMs) >= config.totalTimeoutMs) {
            return false;
        }
        if (result.state == AttemptState::Progress) {
            if (result.bytes == 0 || result.bytes > length - offset) {
                return false;
            }
            offset += result.bytes;
            lastProgressAtMs = afterAttemptMs;
            continue;
        }
        if (result.state == AttemptState::Disconnected || result.state == AttemptState::Error) {
            return false;
        }

        if (static_cast<uint32_t>(afterAttemptMs - lastProgressAtMs) >= config.stallTimeoutMs) {
            return false;
        }
        wait(config.retryDelayMs);
    }

    return true;
}

} // namespace WifiDiagnosticsStream
