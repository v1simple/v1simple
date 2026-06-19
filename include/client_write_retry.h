#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace client_write_retry {

struct Config {
    uint8_t maxZeroWriteRetries;
    uint16_t maxWriteWindowMs;
    uint16_t backoffStartMs;
    uint16_t backoffMaxMs;
};

inline constexpr Config kDefaultConfig{
    2,     // maxZeroWriteRetries
    2500,  // maxWriteWindowMs
    1,     // backoffStartMs
    16     // backoffMaxMs
};

inline uint32_t nowMs() {
#if defined(ARDUINO) || defined(UNIT_TEST)
    return millis();
#else
    return 0;
#endif
}

inline uint32_t elapsedMs(uint32_t startMs) {
    return static_cast<uint32_t>(nowMs() - startMs);
}

inline void service(uint16_t backoffMs = 0) {
#if defined(ARDUINO) || defined(UNIT_TEST)
    if (backoffMs > 0) {
        delay(backoffMs);
    }
#else
    (void)backoffMs;
#endif
#if defined(ARDUINO)
    yield();
#endif
}

template <typename ClientT>
bool writeAll(ClientT& client,
              const uint8_t* data,
              size_t size,
              const Config& cfg = kDefaultConfig) {
    if (size == 0) {
        return true;
    }
    if (!data) {
        return false;
    }

    size_t offset = 0;
    uint8_t zeroWriteRetries = 0;
    uint16_t backoffMs = cfg.backoffStartMs;
    const uint32_t startMs = nowMs();

    while (offset < size) {
        const size_t written = client.write(data + offset, size - offset);
        if (written == 0) {
            if (zeroWriteRetries >= cfg.maxZeroWriteRetries) {
                return false;
            }
            ++zeroWriteRetries;

            if (cfg.maxWriteWindowMs > 0) {
                const uint32_t elapsed = elapsedMs(startMs);
                if (elapsed >= cfg.maxWriteWindowMs) {
                    return false;
                }
                uint16_t boundedBackoff = backoffMs;
                const uint32_t remaining = cfg.maxWriteWindowMs - elapsed;
                if (boundedBackoff > remaining) {
                    boundedBackoff = static_cast<uint16_t>(remaining);
                }
                service(boundedBackoff);
            } else {
                service(backoffMs);
            }

            if (backoffMs < cfg.backoffMaxMs) {
                const uint16_t doubled = static_cast<uint16_t>(backoffMs * 2);
                backoffMs = (doubled > cfg.backoffMaxMs) ? cfg.backoffMaxMs : doubled;
            }
            continue;
        }

        offset += written;
        zeroWriteRetries = 0;
        backoffMs = cfg.backoffStartMs;

        if (offset < size) {
            service();
        }

        if (cfg.maxWriteWindowMs > 0 &&
            offset < size &&
            elapsedMs(startMs) >= cfg.maxWriteWindowMs) {
            return false;
        }
    }

    return true;
}

}  // namespace client_write_retry
