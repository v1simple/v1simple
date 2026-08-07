#pragma once

#include "settings.h"

enum class AlertBand : uint8_t {
    LASER = 0,
    KA = 1,
    K = 2,
    X = 3
};

enum class AlertDirection : uint8_t {
    AHEAD = 0,
    BEHIND = 1,
    SIDE = 2
};

void audio_set_volume(uint8_t volumePercent);
void play_test_voice();
void play_vol0_beep();
void play_alert_voice(AlertBand band, AlertDirection direction);
void play_frequency_voice(AlertBand band, uint16_t freqMHz, AlertDirection direction,
                          VoiceAlertMode mode, bool includeDirection, uint8_t bogeyCount = 1);
void play_direction_only(AlertDirection direction, uint8_t bogeyCount = 0);
void play_threat_escalation(AlertBand band, uint16_t freqMHz, AlertDirection direction,
                            uint8_t total, uint8_t ahead, uint8_t behind, uint8_t side);
void play_band_only(AlertBand band);
void audio_init_sd();
void audio_init_buffers();
void audio_process_amp_timeout();
