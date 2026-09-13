#pragma once

#include <stddef.h>
#include <stdint.h>

namespace WifiExactBodyLengthPolicy {

enum class StartStatus : uint8_t { Ready = 0, Invalid, TooLarge };

struct StartDecision {
    StartStatus status = StartStatus::Invalid;
    size_t expected = 0;
    // The patched framework consumes this value supplied by the raw callback,
    // never its heap-reparsed Content-Length.
    size_t frameworkReadLimit = 0;
};

inline StartDecision begin(const bool preflightValid, const size_t preflightLength,
                           const int frameworkLength, const size_t routeLimit) {
    StartDecision result;
    if (!preflightValid || frameworkLength < 0 ||
        static_cast<size_t>(frameworkLength) != preflightLength) return result;
    if (preflightLength > routeLimit) {
        result.status = StartStatus::TooLarge;
        return result;
    }
    result.status = StartStatus::Ready;
    result.expected = preflightLength;
    result.frameworkReadLimit = preflightLength;
    return result;
}

inline bool acceptsChunk(const size_t expected, const size_t receivedBefore,
                         const size_t currentSize, const size_t frameworkTotal) {
    return receivedBefore <= expected && currentSize <= expected - receivedBefore &&
           frameworkTotal == receivedBefore + currentSize;
}

inline bool acceptsEnd(const size_t expected, const size_t received,
                       const size_t frameworkTotal) {
    return received == expected && frameworkTotal == expected;
}

} // namespace WifiExactBodyLengthPolicy
