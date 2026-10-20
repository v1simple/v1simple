#pragma once

#include <atomic>
#include <cstdint>

// The caller retains this receipt until state leaves Pending. Started means
// samples were submitted: even a later write failure must not replay the job.
// Unavailable is terminal (missing clips/buffers); FailedBeforeStart permits a
// fresh attempt only if the caller still wants this announcement.
struct AudioPlaybackReceipt {
    enum class State : uint8_t { Pending, Started, FailedBeforeStart, Unavailable };
    std::atomic<State> state{State::Unavailable};
};
