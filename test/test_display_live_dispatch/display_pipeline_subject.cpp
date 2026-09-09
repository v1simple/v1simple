// Link the production owner pipeline beside the real painters in this suite.
// Separate translation units retain the production private helper scopes.
#define DISPLAY_WAVESHARE_349 1
#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/settings.h"
#include "../mocks/display_driver.h"
#include "../../src/packet_parser.h"
#include "../../src/display.h"

#include "../../src/modules/volume_fade/volume_fade_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/voice/voice_module.cpp"
#include "../../src/modules/alert_persistence/alert_persistence_module.cpp"
#include "../../src/modules/alp/alp_event_latch.cpp"
#include "../../src/modules/display/render_frame_composer.cpp"
#include "../../src/modules/display/display_pipeline_module.cpp"

// Audio output is outside the display/transfer observation boundary.
AudioPlaybackResult try_play_frequency_voice(AlertBand, uint16_t, AlertDirection, VoiceAlertMode, bool, uint8_t) {
    return AudioPlaybackResult::Unavailable;
}
AudioPlaybackResult try_play_direction_only(AlertDirection, uint8_t) {
    return AudioPlaybackResult::Unavailable;
}
AudioPlaybackResult try_play_threat_escalation(AlertBand, uint16_t, AlertDirection, uint8_t, uint8_t, uint8_t,
                                               uint8_t) {
    return AudioPlaybackResult::Unavailable;
}
