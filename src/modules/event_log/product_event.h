#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "packet_parser_types.h"

constexpr size_t kProductEventMaxV1Alerts = 15;

enum class ProductEventSource : uint8_t { V1 = 0, ALP, SYS };

enum class ProductEventKind : uint8_t {
    BEGIN = 0,
    CHANGE_EVENT,
    END,
    STATE,
    DETECT,
    GUN,
    LINK_LOST,
    LINK_RESTORED,
    GAP,
};

enum class ProductAlpState : uint8_t { UNKNOWN = 0, TARGETED, DLI, LID };

struct ProductV1Alert {
    uint32_t frequency;
    uint8_t band;
    uint8_t direction;
    uint8_t frontStrength;
    uint8_t rearStrength;
    uint8_t priority;
};

struct ProductV1Snapshot {
    uint8_t count;
    ProductV1Alert alerts[kProductEventMaxV1Alerts];
};

struct ProductAlpData {
    ProductAlpState state;
    uint8_t direction;
    uint8_t gun;
    uint8_t raw[3];
};

struct ProductGapData {
    uint32_t lost;
    uint32_t firstMs;
    uint32_t lastMs;
};

struct ProductEvent {
    uint32_t ms;
    uint32_t id;
    uint32_t sequence;
    ProductEventSource source;
    ProductEventKind kind;
    union {
        ProductV1Snapshot v1;
        ProductAlpData alp;
        ProductGapData gap;
    } data;
};

static_assert(std::is_trivially_copyable<ProductV1Alert>::value, "V1 event row must remain trivially copyable");
static_assert(std::is_trivially_copyable<ProductEvent>::value, "queued product events must remain trivially copyable");

struct AlpProductObservation {
    bool connected;
    bool active;
    ProductAlpState state;
    uint8_t direction;
    uint8_t gun;
    uint32_t detectGeneration;
    uint8_t detectRaw[3];
};

class ProductEventBuilder {
  public:
    using Emit = bool (*)(const ProductEvent& event, void* context);

    void begin(Emit emit, void* context) {
        emit_ = emit;
        emitContext_ = context;
        resetSources();
    }

    void resetSources() {
        v1Snapshot_ = ProductV1Snapshot{};
        v1Active_ = false;
        v1LinkLost_ = false;
        v1Id_ = 0;
        v1Sequence_ = 0;
        v1Disabled_ = false;

        alpSeen_ = false;
        alpConnected_ = false;
        alpActive_ = false;
        alpLinkLost_ = false;
        alpId_ = 0;
        alpSequence_ = 0;
        alpDisabled_ = false;
        alpLastState_ = ProductAlpState::UNKNOWN;
        alpLastDirection_ = 0;
        alpLastGun_ = 0;
        alpLastDetectGeneration_ = 0;
        alpHasDetect_ = false;
        std::memset(alpLastDetectRaw_, 0, sizeof(alpLastDetectRaw_));
    }

    void observeV1Table(const AlertData* alerts, size_t count, uint8_t priorityIndex, uint32_t nowMs) {
        if (v1Disabled_) {
            return;
        }
        ProductV1Snapshot next{};
        next.count = static_cast<uint8_t>(count > kProductEventMaxV1Alerts ? kProductEventMaxV1Alerts : count);
        for (size_t i = 0; i < next.count; ++i) {
            const AlertData& input = alerts[i];
            ProductV1Alert& output = next.alerts[i];
            output.frequency = input.frequency;
            output.band = static_cast<uint8_t>(input.band);
            output.direction = static_cast<uint8_t>(input.direction);
            output.frontStrength = input.frontStrength;
            output.rearStrength = input.rearStrength;
            output.priority = (i == priorityIndex) ? 1 : 0;
        }

        if (next.count == 0) {
            if (v1Active_) {
                ProductEvent event = makeEvent(ProductEventSource::V1, ProductEventKind::END, nowMs, v1Id_,
                                               nextSequence(v1Sequence_, v1Disabled_));
                if (!v1Disabled_) {
                    emit(event);
                }
            }
            v1Snapshot_ = ProductV1Snapshot{};
            v1Active_ = false;
            return;
        }

        if (!v1Active_) {
            if (!nextEncounter(v1Id_, v1Sequence_, v1Disabled_)) {
                return;
            }
            ProductEvent event = makeEvent(ProductEventSource::V1, ProductEventKind::BEGIN, nowMs, v1Id_, v1Sequence_);
            event.data.v1 = next;
            emit(event);
            v1Snapshot_ = next;
            v1Active_ = true;
            return;
        }

        if (std::memcmp(&next, &v1Snapshot_, sizeof(next)) == 0) {
            return;
        }
        ProductEvent event = makeEvent(ProductEventSource::V1, ProductEventKind::CHANGE_EVENT, nowMs, v1Id_,
                                       nextSequence(v1Sequence_, v1Disabled_));
        if (!v1Disabled_) {
            event.data.v1 = next;
            emit(event);
        }
        v1Snapshot_ = next;
    }

    void observeV1Link(bool connected, uint32_t nowMs) {
        if (v1Disabled_) {
            return;
        }
        if (!connected && v1Active_ && !v1LinkLost_) {
            ProductEvent event = makeEvent(ProductEventSource::V1, ProductEventKind::LINK_LOST, nowMs, v1Id_,
                                           nextSequence(v1Sequence_, v1Disabled_));
            if (!v1Disabled_) {
                emit(event);
                v1LinkLost_ = true;
            }
        } else if (connected && v1LinkLost_) {
            ProductEvent event = makeEvent(ProductEventSource::V1, ProductEventKind::LINK_RESTORED, nowMs, v1Id_,
                                           nextSequence(v1Sequence_, v1Disabled_));
            if (!v1Disabled_) {
                emit(event);
                v1LinkLost_ = false;
            }
        }
    }

    void observeAlp(const AlpProductObservation& observation, uint32_t nowMs) {
        if (alpDisabled_) {
            return;
        }

        if (alpSeen_) {
            if (!observation.connected && alpConnected_ && alpActive_ && !alpLinkLost_) {
                emitAlp(ProductEventKind::LINK_LOST, observation, nowMs);
                alpLinkLost_ = true;
            } else if (observation.connected && !alpConnected_ && alpLinkLost_) {
                emitAlp(ProductEventKind::LINK_RESTORED, observation, nowMs);
                alpLinkLost_ = false;
            }
        }
        alpSeen_ = true;
        alpConnected_ = observation.connected;

        if (!observation.active) {
            if (alpActive_) {
                emitAlp(ProductEventKind::END, observation, nowMs);
            }
            alpActive_ = false;
            alpLastState_ = ProductAlpState::UNKNOWN;
            alpLastDirection_ = 0;
            alpLastGun_ = 0;
            alpHasDetect_ = false;
            alpLastDetectGeneration_ = observation.detectGeneration;
            return;
        }

        if (!alpActive_) {
            if (!nextEncounter(alpId_, alpSequence_, alpDisabled_)) {
                return;
            }
            alpLastState_ = observation.state;
            alpLastDirection_ = observation.direction;
            alpLastGun_ = 0;
            alpHasDetect_ = false;
            ProductEvent beginEvent = makeEvent(ProductEventSource::ALP, ProductEventKind::BEGIN, nowMs, alpId_,
                                                alpSequence_);
            beginEvent.data.alp = alpData(observation);
            emit(beginEvent);
            alpActive_ = true;
        }

        if (observation.detectGeneration != 0 && observation.detectGeneration != alpLastDetectGeneration_) {
            const bool sameDetect = alpHasDetect_ && std::memcmp(alpLastDetectRaw_, observation.detectRaw, 3) == 0;
            alpLastDetectGeneration_ = observation.detectGeneration;
            if (!sameDetect) {
                emitAlp(ProductEventKind::DETECT, observation, nowMs);
                std::memcpy(alpLastDetectRaw_, observation.detectRaw, 3);
                alpHasDetect_ = true;
            }
        }

        if (observation.gun != 0 && observation.gun != alpLastGun_) {
            emitAlp(ProductEventKind::GUN, observation, nowMs);
            alpLastGun_ = observation.gun;
        }

        const ProductAlpState effectiveState = observation.state == ProductAlpState::UNKNOWN
                                                   ? alpLastState_
                                                   : observation.state;
        if ((effectiveState != ProductAlpState::UNKNOWN && effectiveState != alpLastState_) ||
            observation.direction != alpLastDirection_) {
            AlpProductObservation changed = observation;
            changed.state = effectiveState;
            emitAlp(ProductEventKind::STATE, changed, nowMs);
            alpLastState_ = effectiveState;
            alpLastDirection_ = observation.direction;
        }
    }

    void closeActive(uint32_t nowMs) {
        if (v1Active_ && !v1Disabled_) {
            ProductEvent event = makeEvent(ProductEventSource::V1, ProductEventKind::END, nowMs, v1Id_,
                                           nextSequence(v1Sequence_, v1Disabled_));
            if (!v1Disabled_) {
                emit(event);
            }
        }
        v1Active_ = false;

        if (alpActive_ && !alpDisabled_) {
            AlpProductObservation observation{};
            observation.state = alpLastState_;
            observation.direction = alpLastDirection_;
            emitAlp(ProductEventKind::END, observation, nowMs);
        }
        alpActive_ = false;
    }

  private:
    static ProductEvent makeEvent(ProductEventSource source, ProductEventKind kind, uint32_t nowMs, uint32_t id,
                                  uint32_t sequence) {
        ProductEvent event{};
        event.ms = nowMs;
        event.id = id;
        event.sequence = sequence;
        event.source = source;
        event.kind = kind;
        return event;
    }

    static ProductAlpData alpData(const AlpProductObservation& observation) {
        ProductAlpData data{};
        data.state = observation.state;
        data.direction = observation.direction;
        data.gun = observation.gun;
        std::memcpy(data.raw, observation.detectRaw, sizeof(data.raw));
        return data;
    }

    static uint32_t nextSequence(uint32_t& sequence, bool& disabled) {
        if (sequence == UINT32_MAX) {
            disabled = true;
            return 0;
        }
        return ++sequence;
    }

    static bool nextEncounter(uint32_t& id, uint32_t& sequence, bool& disabled) {
        if (id == UINT32_MAX) {
            disabled = true;
            return false;
        }
        ++id;
        sequence = 1;
        return true;
    }

    void emitAlp(ProductEventKind kind, const AlpProductObservation& observation, uint32_t nowMs) {
        const uint32_t sequence = nextSequence(alpSequence_, alpDisabled_);
        if (alpDisabled_) {
            return;
        }
        ProductEvent event = makeEvent(ProductEventSource::ALP, kind, nowMs, alpId_, sequence);
        event.data.alp = alpData(observation);
        emit(event);
    }

    void emit(const ProductEvent& event) {
        if (emit_) {
            (void)emit_(event, emitContext_);
        }
    }

    Emit emit_ = nullptr;
    void* emitContext_ = nullptr;

    ProductV1Snapshot v1Snapshot_{};
    bool v1Active_ = false;
    bool v1LinkLost_ = false;
    uint32_t v1Id_ = 0;
    uint32_t v1Sequence_ = 0;
    bool v1Disabled_ = false;

    bool alpSeen_ = false;
    bool alpConnected_ = false;
    bool alpActive_ = false;
    bool alpLinkLost_ = false;
    uint32_t alpId_ = 0;
    uint32_t alpSequence_ = 0;
    bool alpDisabled_ = false;
    ProductAlpState alpLastState_ = ProductAlpState::UNKNOWN;
    uint8_t alpLastDirection_ = 0;
    uint8_t alpLastGun_ = 0;
    uint32_t alpLastDetectGeneration_ = 0;
    bool alpHasDetect_ = false;
    uint8_t alpLastDetectRaw_[3] = {};
};
