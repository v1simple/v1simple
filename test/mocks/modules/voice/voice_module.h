// Mock voice_module.h for native unit testing
#pragma once

#include "packet_parser.h"
#include "../../audio_beep.h"

struct VoiceContext {
    const AlertData* alerts = nullptr;
    int alertCount = 0;
    const AlertData* priority = nullptr;
    bool isMuted = false;
    bool isSoftMuted = false;
    bool isProxyConnected = false;
    uint8_t mainVolume = 0;
    bool isSuppressed = false;
    unsigned long now = 0;
};

struct VoiceAction {
    enum class Type : uint8_t {
        NONE = 0,
        ANNOUNCE_PRIORITY,
        ANNOUNCE_DIRECTION,
        ANNOUNCE_SECONDARY,
        ANNOUNCE_ESCALATION
    };

    Type type = Type::NONE;
    AlertBand band = AlertBand::KA;
    uint16_t freq = 0;
    AlertDirection dir = AlertDirection::AHEAD;
    uint8_t bogeyCount = 0;
    uint8_t aheadCount = 0;
    uint8_t behindCount = 0;
    uint8_t sideCount = 0;

    bool hasAction() const { return type != Type::NONE; }
};

class VoiceModule {
public:
    int clearAllStateCalls = 0;
    int processCalls = 0;
    float mockSpeedMph = 0.0f;
    bool mockHasValidSpeed = false;
    VoiceContext lastContext{};
    VoiceAction nextAction{};
    
    void resetMock() {
        clearAllStateCalls = 0;
        processCalls = 0;
        mockSpeedMph = 0.0f;
        mockHasValidSpeed = false;
        lastContext = VoiceContext{};
        nextAction = VoiceAction{};
    }
    
    float getCurrentSpeedMph(unsigned long /*now*/) { return mockSpeedMph; }
    bool hasValidSpeedSource(unsigned long /*now*/) const { return mockHasValidSpeed; }
    void reset() { clearAllState(); }  // Alias for consistency with other modules
    void clearAllState() { clearAllStateCalls++; }
    VoiceAction process(const VoiceContext& ctx) {
        ++processCalls;
        lastContext = ctx;
        return nextAction;
    }
};
