// Mock volume_fade_module.h for native unit testing
#pragma once

struct VolumeFadeContext {
    bool hasAlert = false;
    bool alertMuted = false;
    bool alertSuppressed = false;
    uint8_t currentVolume = 0;
    uint8_t currentMuteVolume = 0;
    uint16_t currentFrequency = 0;
    unsigned long now = 0;
};

struct VolumeFadeAction {
    enum class Type : uint8_t {
        NONE = 0,
        FADE_DOWN,
        RESTORE
    };

    Type type = Type::NONE;
    uint8_t targetVolume = 0;
    uint8_t targetMuteVolume = 0;
    uint8_t restoreVolume = 0;
    uint8_t restoreMuteVolume = 0;

    bool hasAction() const { return type != Type::NONE; }
};

class VolumeFadeModule {
public:
    bool tracking = false;
    int processCalls = 0;
    int setBaselineHintCalls = 0;
    VolumeFadeContext lastContext{};
    VolumeFadeAction nextAction{};
    uint8_t lastHintVolume = 0;
    uint8_t lastHintMuteVolume = 0;
    uint32_t lastHintNowMs = 0;
    
    void reset() {
        tracking = false;
        processCalls = 0;
        setBaselineHintCalls = 0;
        lastContext = VolumeFadeContext{};
        nextAction = VolumeFadeAction{};
        lastHintVolume = 0;
        lastHintMuteVolume = 0;
        lastHintNowMs = 0;
    }
    
    bool isTracking() const { return tracking; }

    VolumeFadeAction process(const VolumeFadeContext& ctx) {
        ++processCalls;
        lastContext = ctx;
        return nextAction;
    }

    void setBaselineHint(uint8_t mainVol, uint8_t muteVol, uint32_t nowMs) {
        ++setBaselineHintCalls;
        lastHintVolume = mainVol;
        lastHintMuteVolume = muteVol;
        lastHintNowMs = nowMs;
    }
};
