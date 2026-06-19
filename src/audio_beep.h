// audio_beep.h
#pragma once

#include <stdint.h>
#include "settings.h"  // For VoiceAlertMode enum

// Band types for voice alerts
enum class AlertBand : uint8_t {
    LASER = 0,
    KA = 1,
    K = 2,
    X = 3
};

// Direction types for voice alerts
enum class AlertDirection : uint8_t {
    AHEAD = 0,
    BEHIND = 1,
    SIDE = 2
};

// Set audio volume (0-100%)
void audio_set_volume(uint8_t volumePercent);

// Play "Test" for volume confirmation
void play_test_voice();

// Call to play a beep for VOL 0 warning
void play_vol0_beep();

// Play voice alert for a specific band and direction
// Returns immediately if already playing or audio disabled
void play_alert_voice(AlertBand band, AlertDirection direction);

// Play frequency announcement from SD card audio clips
// Format depends on mode:
//   BAND_ONLY: "Ka"
//   FREQ_ONLY: "34 7 49"
//   BAND_FREQ: "Ka 34 7 49"
// direction appended if includeDirection is true: "ahead", "behind", "side"
// bogeyCount appended if > 1: "2 bogeys", "3 bogeys", etc.
// freqMHz: frequency in MHz (e.g., 34749 for 34.749 GHz)
// Returns immediately if already playing, audio disabled, or SD not available
void play_frequency_voice(AlertBand band, uint16_t freqMHz, AlertDirection direction,
                          VoiceAlertMode mode, bool includeDirection, uint8_t bogeyCount = 1);

// Play direction-only announcement (used when same alert changes direction)
// Says "ahead", "behind", or "side", optionally with bogey count if > 1
void play_direction_only(AlertDirection direction, uint8_t bogeyCount = 0);

// Play threat escalation announcement with full context:
// "[Band] [freq] [direction] [N] bogeys, [X] ahead, [Y] behind"
// Used when a secondary alert ramps up from weak (≤2 bars) to strong (≥4 bars)
void play_threat_escalation(AlertBand band, uint16_t freqMHz, AlertDirection direction,
                            uint8_t total, uint8_t ahead, uint8_t behind, uint8_t side);

// Play band-only announcement (e.g., "Ka", "K", "X", "Laser")
void play_band_only(AlertBand band);

// Initialize SD audio (call after storage manager is ready)
void audio_init_sd();

// Allocate audio decode buffers in PSRAM (call once from setup, before audio_init_sd).
// Frees ~5 KiB of internal .bss to preserve contiguous DMA headroom.
void audio_init_buffers();

// Process amp warm timeout - call from main loop
// Disables speaker amp after 3 seconds of inactivity to save power
void audio_process_amp_timeout();
