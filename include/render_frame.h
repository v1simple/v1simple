/**
 * RenderFrame - typed display contract between source composition and pixels.
 *
 * The display pipeline composes one frame that describes the chosen primary
 * owner plus V1 secondary alert context. V1Display consumes this contract through
 * renderFrame() and keeps the legacy update() paths behind that single entry.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../src/modules/alp/alp_laser_event.h"
#include "../src/packet_parser_types.h"

constexpr size_t kRenderFrameMaxCards = 15;

enum class RenderFramePrimaryKind : uint8_t {
    NONE = 0,
    IDLE,
    V1_LIVE,
    V1_PERSISTED,
    ALP_LIVE,
    ALP_PERSISTED,
};

struct RenderFrameCard {
    enum class Kind : uint8_t { V1, ALP };

    Kind kind = Kind::V1;
    AlertData v1Alert{};
    AlpLaserEvent alpEvent{};
};

struct RenderFrame {
    static constexpr size_t MAX_CARDS = kRenderFrameMaxCards;

    RenderFramePrimaryKind primaryKind = RenderFramePrimaryKind::NONE;
    AlertData v1Priority{};
    AlpLaserEvent alpPrimary{};
    DisplayState primaryState{};
    std::array<RenderFrameCard, MAX_CARDS> cards{};
    int cardCount = 0;
    DisplayState context{};

    // Stealth mode: populated when primaryKind==IDLE and stealth is enabled.
    // V1Display renders a blank screen with speed centered instead of the normal idle UI.
    bool stealthMode = false;
    float stealthSpeedMph = 0.0f;
    bool stealthSpeedValid = false;
};