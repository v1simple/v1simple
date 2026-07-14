#include "display_correctness_trace.h"

namespace {

DisplayCorrectnessTraceLog gDisplayCorrectnessTraceLog;
uint32_t gDisplayCorrectnessTraceSeq = 0;

DisplayCorrectnessStatus compareStatus(bool applicable, uint32_t source, uint32_t rendered) {
    if (!applicable) {
        return DisplayCorrectnessStatus::NOT_APPLICABLE;
    }
    return source == rendered ? DisplayCorrectnessStatus::MATCH : DisplayCorrectnessStatus::MISMATCH;
}

DisplayCorrectnessOwner ownerFor(RenderFramePrimaryKind kind) {
    switch (kind) {
    case RenderFramePrimaryKind::IDLE:
        return DisplayCorrectnessOwner::IDLE;
    case RenderFramePrimaryKind::V1_LIVE:
    case RenderFramePrimaryKind::V1_PERSISTED:
        return DisplayCorrectnessOwner::V1;
    case RenderFramePrimaryKind::ALP_LIVE:
    case RenderFramePrimaryKind::ALP_PERSISTED:
        return DisplayCorrectnessOwner::ALP;
    case RenderFramePrimaryKind::NONE:
    default:
        return DisplayCorrectnessOwner::NONE;
    }
}

uint8_t primaryBandFor(const RenderFrame& frame) {
    switch (frame.primaryKind) {
    case RenderFramePrimaryKind::V1_LIVE:
    case RenderFramePrimaryKind::V1_PERSISTED:
        return static_cast<uint8_t>(frame.v1Priority.band);
    case RenderFramePrimaryKind::ALP_LIVE:
    case RenderFramePrimaryKind::ALP_PERSISTED:
        return BAND_LASER;
    case RenderFramePrimaryKind::IDLE:
        return frame.primaryState.activeBands;
    case RenderFramePrimaryKind::NONE:
    default:
        return BAND_NONE;
    }
}

uint8_t primaryArrowsFor(const RenderFrame& frame) {
    switch (frame.primaryKind) {
    case RenderFramePrimaryKind::V1_PERSISTED:
        return static_cast<uint8_t>(frame.v1Priority.direction);
    case RenderFramePrimaryKind::ALP_LIVE:
    case RenderFramePrimaryKind::ALP_PERSISTED:
        return static_cast<uint8_t>(frame.primaryState.priorityArrow);
    case RenderFramePrimaryKind::V1_LIVE:
    case RenderFramePrimaryKind::IDLE:
        return static_cast<uint8_t>(frame.primaryState.arrows);
    case RenderFramePrimaryKind::NONE:
    default:
        return DIR_NONE;
    }
}

uint32_t primaryFrequencyFor(const RenderFrame& frame) {
    switch (frame.primaryKind) {
    case RenderFramePrimaryKind::V1_LIVE:
    case RenderFramePrimaryKind::V1_PERSISTED:
        return frame.v1Priority.frequency;
    case RenderFramePrimaryKind::IDLE:
    case RenderFramePrimaryKind::ALP_LIVE:
    case RenderFramePrimaryKind::ALP_PERSISTED:
    case RenderFramePrimaryKind::NONE:
    default:
        return 0;
    }
}

uint8_t renderedSignalBarsFor(const RenderFrame& frame) {
    switch (frame.primaryKind) {
    case RenderFramePrimaryKind::V1_PERSISTED:
        return 0;
    case RenderFramePrimaryKind::V1_LIVE:
    case RenderFramePrimaryKind::ALP_LIVE:
    case RenderFramePrimaryKind::ALP_PERSISTED:
    case RenderFramePrimaryKind::IDLE:
        return frame.primaryState.signalBars;
    case RenderFramePrimaryKind::NONE:
    default:
        return 0;
    }
}

uint8_t sourceSignalBarsFor(const RenderFrame& frame) {
    switch (frame.primaryKind) {
    case RenderFramePrimaryKind::V1_LIVE:
        return frame.context.signalBars;
    case RenderFramePrimaryKind::V1_PERSISTED:
        return 0;
    case RenderFramePrimaryKind::ALP_LIVE:
    case RenderFramePrimaryKind::ALP_PERSISTED:
    case RenderFramePrimaryKind::IDLE:
        return frame.primaryState.signalBars;
    case RenderFramePrimaryKind::NONE:
    default:
        return 0;
    }
}

bool renderedMutedFor(const RenderFrame& frame) {
    switch (frame.primaryKind) {
    case RenderFramePrimaryKind::V1_LIVE:
    case RenderFramePrimaryKind::ALP_LIVE:
    case RenderFramePrimaryKind::ALP_PERSISTED:
        return frame.primaryState.muted;
    case RenderFramePrimaryKind::IDLE:
    case RenderFramePrimaryKind::V1_PERSISTED:
    case RenderFramePrimaryKind::NONE:
    default:
        return false;
    }
}

bool hasPrimaryAlert(RenderFramePrimaryKind kind) {
    return kind == RenderFramePrimaryKind::V1_LIVE || kind == RenderFramePrimaryKind::V1_PERSISTED ||
           kind == RenderFramePrimaryKind::ALP_LIVE || kind == RenderFramePrimaryKind::ALP_PERSISTED;
}

} // namespace

DisplayCorrectnessTraceEvent buildDisplayCorrectnessTraceEvent(const RenderFrame& frame, uint32_t tsMs) {
    DisplayCorrectnessTraceEvent event;
    event.seq = ++gDisplayCorrectnessTraceSeq;
    event.tsMs = tsMs;
    event.primaryKind = frame.primaryKind;
    event.owner = ownerFor(frame.primaryKind);
    event.cardCount =
        frame.cardCount < 0
            ? 0
            : static_cast<uint8_t>(frame.cardCount > static_cast<int>(RenderFrame::MAX_CARDS) ? RenderFrame::MAX_CARDS
                                                                                              : frame.cardCount);

    event.sourceBand = primaryBandFor(frame);
    event.renderedBand = primaryBandFor(frame);
    event.sourceArrows = primaryArrowsFor(frame);
    event.renderedArrows = primaryArrowsFor(frame);
    event.sourceFrequency = primaryFrequencyFor(frame);
    event.renderedFrequency = primaryFrequencyFor(frame);
    event.sourceBogey = frame.context.bogeyCounterChar;
    event.renderedBogey = frame.primaryState.bogeyCounterChar;
    event.sourceMuted = frame.context.muted;
    event.renderedMuted = renderedMutedFor(frame);
    event.sourceSignalBars = sourceSignalBarsFor(frame);
    event.renderedSignalBars = renderedSignalBarsFor(frame);

    const bool primaryAlert = hasPrimaryAlert(frame.primaryKind);
    event.bandStatus = compareStatus(primaryAlert, event.sourceBand, event.renderedBand);
    event.arrowsStatus = compareStatus(primaryAlert, event.sourceArrows, event.renderedArrows);
    event.frequencyStatus = compareStatus(frame.primaryKind == RenderFramePrimaryKind::V1_LIVE ||
                                              frame.primaryKind == RenderFramePrimaryKind::V1_PERSISTED,
                                          event.sourceFrequency, event.renderedFrequency);
    event.bogeyStatus =
        compareStatus(frame.primaryKind != RenderFramePrimaryKind::NONE, static_cast<uint8_t>(event.sourceBogey),
                      static_cast<uint8_t>(event.renderedBogey));
    event.muteStatus = compareStatus(frame.primaryKind == RenderFramePrimaryKind::V1_LIVE, event.sourceMuted ? 1u : 0u,
                                     event.renderedMuted ? 1u : 0u);
    event.signalBarsStatus = compareStatus(frame.primaryKind == RenderFramePrimaryKind::V1_LIVE ||
                                               frame.primaryKind == RenderFramePrimaryKind::ALP_LIVE ||
                                               frame.primaryKind == RenderFramePrimaryKind::ALP_PERSISTED,
                                           event.sourceSignalBars, event.renderedSignalBars);

    return event;
}

void DisplayCorrectnessTraceLog::lock() const {
#ifdef UNIT_TEST
    while (lockFlag_.test_and_set(std::memory_order_acquire)) {
    }
#else
    portENTER_CRITICAL(&lockMux_);
#endif
}

void DisplayCorrectnessTraceLog::unlock() const {
#ifdef UNIT_TEST
    lockFlag_.clear(std::memory_order_release);
#else
    portEXIT_CRITICAL(&lockMux_);
#endif
}

void DisplayCorrectnessTraceLog::reset() {
    LockGuard guard(*this);
    head_ = 0;
    tail_ = 0;
    count_ = 0;
    published_ = 0;
    drops_ = 0;
}

bool DisplayCorrectnessTraceLog::publish(const DisplayCorrectnessTraceEvent& event) {
    LockGuard guard(*this);
    bool dropped = false;
    if (count_ == kCapacity) {
        tail_ = nextIndex(tail_);
        count_--;
        drops_++;
        dropped = true;
    }

    ring_[head_] = event;
    head_ = nextIndex(head_);
    count_++;
    published_++;
    return !dropped;
}

size_t DisplayCorrectnessTraceLog::copyRecent(DisplayCorrectnessTraceEvent* out, size_t maxCount) const {
    LockGuard guard(*this);
    if (!out || maxCount == 0 || count_ == 0) {
        return 0;
    }

    const size_t copyCount = maxCount < count_ ? maxCount : count_;
    uint8_t idx = head_;
    for (size_t i = 0; i < copyCount; ++i) {
        idx = prevIndex(idx);
        out[i] = ring_[idx];
    }
    return copyCount;
}

DisplayCorrectnessTraceStats DisplayCorrectnessTraceLog::stats() const {
    LockGuard guard(*this);
    DisplayCorrectnessTraceStats out;
    out.published = published_;
    out.drops = drops_;
    out.size = count_;
    return out;
}

bool displayCorrectnessTracePublish(const DisplayCorrectnessTraceEvent& event) {
    return gDisplayCorrectnessTraceLog.publish(event);
}

size_t displayCorrectnessTraceCopyRecent(DisplayCorrectnessTraceEvent* out, size_t maxCount) {
    return gDisplayCorrectnessTraceLog.copyRecent(out, maxCount);
}

DisplayCorrectnessTraceStats displayCorrectnessTraceStats() {
    return gDisplayCorrectnessTraceLog.stats();
}

void displayCorrectnessTraceReset() {
    gDisplayCorrectnessTraceLog.reset();
    gDisplayCorrectnessTraceSeq = 0;
}
