#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef UNIT_TEST
#include <atomic>
#else
#include <freertos/FreeRTOS.h>
#endif

#include "render_frame.h"

// Bounded, in-memory trace of the display pipeline's parsed-source -> rendered
// frame contract. This is intentionally not SD/Serial logging: publishing is
// fixed-size, heap-free, and drops the oldest trace when full so correctness
// instrumentation can never block BLE/display work.
enum class DisplayCorrectnessOwner : uint8_t {
    NONE = 0,
    IDLE = 1,
    V1 = 2,
    ALP = 3,
};

enum class DisplayCorrectnessStatus : uint8_t {
    NOT_APPLICABLE = 0,
    MATCH = 1,
    MISMATCH = 2,
};

struct DisplayCorrectnessTraceEvent {
    uint32_t seq = 0;
    uint32_t tsMs = 0;
    RenderFramePrimaryKind primaryKind = RenderFramePrimaryKind::NONE;
    DisplayCorrectnessOwner owner = DisplayCorrectnessOwner::NONE;
    uint8_t cardCount = 0;

    uint8_t sourceBand = BAND_NONE;
    uint8_t renderedBand = BAND_NONE;
    uint8_t sourceArrows = DIR_NONE;
    uint8_t renderedArrows = DIR_NONE;
    uint32_t sourceFrequency = 0;
    uint32_t renderedFrequency = 0;
    char sourceBogey = '\0';
    char renderedBogey = '\0';
    bool sourceMuted = false;
    bool renderedMuted = false;
    uint8_t sourceSignalBars = 0;
    uint8_t renderedSignalBars = 0;

    DisplayCorrectnessStatus bandStatus = DisplayCorrectnessStatus::NOT_APPLICABLE;
    DisplayCorrectnessStatus arrowsStatus = DisplayCorrectnessStatus::NOT_APPLICABLE;
    DisplayCorrectnessStatus frequencyStatus = DisplayCorrectnessStatus::NOT_APPLICABLE;
    DisplayCorrectnessStatus bogeyStatus = DisplayCorrectnessStatus::NOT_APPLICABLE;
    DisplayCorrectnessStatus muteStatus = DisplayCorrectnessStatus::NOT_APPLICABLE;
    DisplayCorrectnessStatus signalBarsStatus = DisplayCorrectnessStatus::NOT_APPLICABLE;
};

struct DisplayCorrectnessTraceStats {
    uint32_t published = 0;
    uint32_t drops = 0;
    size_t size = 0;
};

DisplayCorrectnessTraceEvent buildDisplayCorrectnessTraceEvent(const RenderFrame& frame,
                                                               uint32_t tsMs);

class DisplayCorrectnessTraceLog {
public:
    static constexpr size_t kCapacity = 64;

    void reset();
    bool publish(const DisplayCorrectnessTraceEvent& event);
    size_t copyRecent(DisplayCorrectnessTraceEvent* out, size_t maxCount) const;
    DisplayCorrectnessTraceStats stats() const;

private:
    void lock() const;
    void unlock() const;

    struct LockGuard {
        explicit LockGuard(const DisplayCorrectnessTraceLog& ownerRef) : owner(ownerRef) {
            owner.lock();
        }
        ~LockGuard() {
            owner.unlock();
        }
        const DisplayCorrectnessTraceLog& owner;
    };

    static uint8_t nextIndex(uint8_t idx) {
        return static_cast<uint8_t>((idx + 1u) % kCapacity);
    }
    static uint8_t prevIndex(uint8_t idx) {
        return static_cast<uint8_t>((idx + kCapacity - 1u) % kCapacity);
    }

    DisplayCorrectnessTraceEvent ring_[kCapacity] = {};
    uint8_t head_ = 0;
    uint8_t tail_ = 0;
    uint8_t count_ = 0;
    uint32_t published_ = 0;
    uint32_t drops_ = 0;
#ifdef UNIT_TEST
    mutable std::atomic_flag lockFlag_ = ATOMIC_FLAG_INIT;
#else
    mutable portMUX_TYPE lockMux_ = portMUX_INITIALIZER_UNLOCKED;
#endif
};

bool displayCorrectnessTracePublish(const DisplayCorrectnessTraceEvent& event);
size_t displayCorrectnessTraceCopyRecent(DisplayCorrectnessTraceEvent* out, size_t maxCount);
DisplayCorrectnessTraceStats displayCorrectnessTraceStats();
void displayCorrectnessTraceReset();
