#include "product_event_csv.h"

#include <cstdio>

namespace {

const char* sourceToken(ProductEventSource source) {
    switch (source) {
    case ProductEventSource::V1:
        return "V1";
    case ProductEventSource::ALP:
        return "ALP";
    case ProductEventSource::SYS:
        return "SYS";
    }
    return "SYS";
}

const char* eventToken(ProductEventKind kind) {
    switch (kind) {
    case ProductEventKind::BEGIN:
        return "BEGIN";
    case ProductEventKind::CHANGE_EVENT:
        return "CHANGE";
    case ProductEventKind::END:
        return "END";
    case ProductEventKind::STATE:
        return "STATE";
    case ProductEventKind::DETECT:
        return "DETECT";
    case ProductEventKind::GUN:
        return "GUN";
    case ProductEventKind::LINK_LOST:
        return "LINK_LOST";
    case ProductEventKind::LINK_RESTORED:
        return "LINK_RESTORED";
    case ProductEventKind::GAP:
        return "GAP";
    }
    return "UNKNOWN";
}

const char* bandToken(uint8_t band) {
    switch (static_cast<Band>(band)) {
    case BAND_LASER:
        return "Laser";
    case BAND_KA:
        return "Ka";
    case BAND_K:
        return "K";
    case BAND_X:
        return "X";
    case BAND_KU:
        return "Ku";
    default:
        return "None";
    }
}

const char* v1DirectionToken(uint8_t direction) {
    switch (static_cast<Direction>(direction)) {
    case DIR_FRONT:
        return "F";
    case DIR_SIDE:
        return "S";
    case DIR_REAR:
        return "R";
    default:
        return "U";
    }
}

const char* alpDirectionToken(uint8_t direction) {
    switch (direction) {
    case 1:
        return "F";
    case 2:
        return "R";
    default:
        return "U";
    }
}

const char* alpStateToken(ProductAlpState state) {
    switch (state) {
    case ProductAlpState::TARGETED:
        return "TARGETED";
    case ProductAlpState::DLI:
        return "DLI";
    case ProductAlpState::LID:
        return "LID";
    default:
        return "UNKNOWN";
    }
}

const char* alpGunToken(uint8_t gun) {
    switch (gun) {
    case 1:
        return "PL3_PROLITE";
    case 2:
        return "DRAGONEYE_COMPACT";
    case 3:
        return "LTI_TRUSPEED_LR";
    case 4:
        return "LASER_ATLANTA_PL2";
    case 5:
        return "MARKSMAN_ULTRALYTE";
    case 6:
        return "STALKER_LZ1";
    case 7:
        return "LASER_ALLY";
    case 8:
        return "ATLANTA_STEALTH";
    default:
        return "UNKNOWN";
    }
}

size_t finish(char* output, size_t capacity, int length) {
    if (length <= 0 || static_cast<size_t>(length) >= capacity) {
        if (output && capacity > 0) {
            output[0] = '\0';
        }
        return 0;
    }
    return static_cast<size_t>(length);
}

} // namespace

size_t productEventRowCount(const ProductEvent& event) {
    if (event.source == ProductEventSource::V1 &&
        (event.kind == ProductEventKind::BEGIN || event.kind == ProductEventKind::CHANGE_EVENT)) {
        return event.data.v1.count;
    }
    return 1;
}

size_t serializeProductEventRow(const ProductEvent& event, size_t item, char* output, size_t capacity) {
    if (!output || capacity == 0) {
        return 0;
    }
    const size_t count = productEventRowCount(event);
    if (count == 0 || item >= count) {
        output[0] = '\0';
        return 0;
    }

    char payload[160];
    int payloadLength = 0;
    if (event.source == ProductEventSource::V1 &&
        (event.kind == ProductEventKind::BEGIN || event.kind == ProductEventKind::CHANGE_EVENT)) {
        const ProductV1Alert& alert = event.data.v1.alerts[item];
        payloadLength = std::snprintf(payload, sizeof(payload), "band=%s;freq=%lu;dir=%s;front=%u;rear=%u;priority=%u",
                                      bandToken(alert.band), static_cast<unsigned long>(alert.frequency),
                                      v1DirectionToken(alert.direction), static_cast<unsigned>(alert.frontStrength),
                                      static_cast<unsigned>(alert.rearStrength), static_cast<unsigned>(alert.priority));
    } else if (event.source == ProductEventSource::V1 && event.kind == ProductEventKind::END) {
        payloadLength = std::snprintf(payload, sizeof(payload), "state=EMPTY");
    } else if (event.source == ProductEventSource::V1 && event.kind == ProductEventKind::LINK_LOST) {
        payloadLength = std::snprintf(payload, sizeof(payload), "state=LOST");
    } else if (event.source == ProductEventSource::V1 && event.kind == ProductEventKind::LINK_RESTORED) {
        payloadLength = std::snprintf(payload, sizeof(payload), "state=RESTORED");
    } else if (event.source == ProductEventSource::ALP &&
               (event.kind == ProductEventKind::BEGIN || event.kind == ProductEventKind::STATE)) {
        payloadLength = std::snprintf(payload, sizeof(payload), "state=%s;dir=%s", alpStateToken(event.data.alp.state),
                                      alpDirectionToken(event.data.alp.direction));
    } else if (event.source == ProductEventSource::ALP && event.kind == ProductEventKind::DETECT) {
        payloadLength = std::snprintf(payload, sizeof(payload), "state=%s;raw=%02X%02X%02X;dir=%s",
                                      alpStateToken(event.data.alp.state), event.data.alp.raw[0], event.data.alp.raw[1],
                                      event.data.alp.raw[2], alpDirectionToken(event.data.alp.direction));
    } else if (event.source == ProductEventSource::ALP && event.kind == ProductEventKind::GUN) {
        payloadLength = std::snprintf(payload, sizeof(payload), "type=%s", alpGunToken(event.data.alp.gun));
    } else if (event.source == ProductEventSource::ALP && event.kind == ProductEventKind::END) {
        payloadLength = std::snprintf(payload, sizeof(payload), "state=IDLE");
    } else if (event.source == ProductEventSource::ALP && event.kind == ProductEventKind::LINK_LOST) {
        payloadLength = std::snprintf(payload, sizeof(payload), "state=LOST");
    } else if (event.source == ProductEventSource::ALP && event.kind == ProductEventKind::LINK_RESTORED) {
        payloadLength = std::snprintf(payload, sizeof(payload), "state=RESTORED");
    } else if (event.source == ProductEventSource::SYS && event.kind == ProductEventKind::GAP) {
        payloadLength = std::snprintf(payload, sizeof(payload), "lost=%lu;first_ms=%lu;last_ms=%lu",
                                      static_cast<unsigned long>(event.data.gap.lost),
                                      static_cast<unsigned long>(event.data.gap.firstMs),
                                      static_cast<unsigned long>(event.data.gap.lastMs));
    }

    if (finish(payload, sizeof(payload), payloadLength) == 0) {
        output[0] = '\0';
        return 0;
    }
    return finish(output, capacity,
                  std::snprintf(output, capacity, "%lu,%s,%s,%lu,%lu,%u,%u,%s\n",
                                static_cast<unsigned long>(event.ms), sourceToken(event.source), eventToken(event.kind),
                                static_cast<unsigned long>(event.id), static_cast<unsigned long>(event.sequence),
                                static_cast<unsigned>(item), static_cast<unsigned>(count), payload));
}
