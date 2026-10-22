#pragma once

#include <atomic>
#include <cstdint>

// The caller retains this receipt while Pending or CancelRequested. A request
// is not a cleanup acknowledgement. Started commits the first PCM submission:
// even a later write failure must not replay the job. Unavailable is terminal
// (missing clips/buffers); FailedBeforeStart and CancelledBeforeStart are unheard
// and may be selected afresh only if the caller still wants the announcement.
struct AudioPlaybackReceipt {
    enum class State : uint8_t {
        Pending,
        CancelRequested,
        Started,
        FailedBeforeStart,
        Unavailable,
        CancelledBeforeStart
    };
    std::atomic<State> state{State::Unavailable};

    bool requestCancellation() {
        State expected = State::Pending;
        return state.compare_exchange_strong(expected, State::CancelRequested);
    }

    bool tryStart() {
        State expected = State::Pending;
        return state.compare_exchange_strong(expected, State::Started);
    }

    bool isOwned() const {
        const State current = state.load();
        return current == State::Pending || current == State::CancelRequested;
    }
};
