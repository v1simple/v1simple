#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/display.h"
#include "../mocks/packet_parser.h"
#include "../mocks/settings.h"

#ifndef ARDUINO
SerialClass Serial;
SettingsManager settingsManager;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/alert_persistence/alert_persistence_module.h"
#include "../../src/perf_metrics.h"

PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;
static int g_displayLogCalls = 0;
static char g_lastDisplayLogEvent[24] = "";
static char g_lastDisplayLogDetail[224] = "";
void perfRecordDisplayRenderUs(uint32_t /*us*/) {}
void perfRecordDisplayScenarioRenderUs(uint32_t /*us*/) {}
void perfRecordDisplayVoiceUs(uint32_t /*us*/) {}
void perfSetDisplayRenderScenario(PerfDisplayRenderScenario /*scenario*/) {}
PerfDisplayRenderScenario perfGetDisplayRenderScenario() { return PerfDisplayRenderScenario::None; }
void perfClearDisplayRenderScenario() {}

AlertPersistenceModule::AlertPersistenceModule() = default;

void AlertPersistenceModule::begin(V1BLEClient* ble,
                                   PacketParser* parserRef,
                                   V1Display* displayRef,
                                   SettingsManager* settingsRef) {
    bleClient_ = ble;
    parser_ = parserRef;
    display_ = displayRef;
    settings_ = settingsRef;
}

void AlertPersistenceModule::setPersistedAlert(const AlertData& alert) {
    persistedAlert_ = alert;
}

void AlertPersistenceModule::startPersistence(unsigned long now) {
    if (!alertPersistenceActive_) {
        alertClearedTime_ = now;
        alertPersistenceActive_ = true;
    }
}

void AlertPersistenceModule::clearPersistence() {
    persistedAlert_ = AlertData{};
    alertClearedTime_ = 0;
    alertPersistenceActive_ = false;
}

bool AlertPersistenceModule::shouldShowPersisted(unsigned long now, unsigned long persistMs) const {
    return alertPersistenceActive_ &&
           persistedAlert_.isValid &&
           (now - alertClearedTime_) < persistMs;
}

// ALP module stubs — pipeline calls alpGunAbbrev() and uses AlpRuntimeModule pointer
#include "../../src/modules/alp/alp_runtime_module.h"
#include "../../src/modules/alp/alp_event_latch.h"
const char* alpStateName(AlpState) { return "OFF"; }
const char* alpGunName(AlpGunType) { return "Unknown"; }
const char* alpGunAbbrev(AlpGunType gun) {
    switch (gun) {
        case AlpGunType::MARKSMAN_ULTRALYTE: return "ULT";
        case AlpGunType::PL3_PROLITE:        return "PL3";
        case AlpGunType::DRAGONEYE_COMPACT:  return "DE";
        default:                             return "LASER";
    }
}
const char* alpLaserDirectionName(AlpLaserDirection direction) {
    switch (direction) {
        case AlpLaserDirection::FRONT:   return "FRONT";
        case AlpLaserDirection::REAR:    return "REAR";
        case AlpLaserDirection::UNKNOWN:
        default:                         return "UNKNOWN";
    }
}
AlpGunType alpLookupGun(uint8_t, uint8_t) { return AlpGunType::UNKNOWN; }
AlpGunType alpLookupGunDetect(uint8_t, uint8_t) { return AlpGunType::UNKNOWN; }
// Member-method stub — test does not include alp_runtime_module.cpp
void AlpRuntimeModule::logDisplayDecision(uint32_t, const char* event, const char* detail) {
    g_displayLogCalls++;
    snprintf(g_lastDisplayLogEvent, sizeof(g_lastDisplayLogEvent), "%s", event ? event : "");
    snprintf(g_lastDisplayLogDetail, sizeof(g_lastDisplayLogDetail), "%s", detail ? detail : "");
}

#include "../../src/modules/speed_mute/speed_mute_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/alp/alp_event_latch.cpp"
#include "../../src/modules/display/render_frame_composer.cpp"
#include "../../src/modules/display/display_pipeline_module.cpp"

VoiceModule::VoiceModule() {}
void VoiceModule::begin(SettingsManager*, V1BLEClient*) {}
void VoiceModule::clearAllState() {}
VoiceAction VoiceModule::process(const VoiceContext&) { return VoiceAction{}; }

void play_frequency_voice(AlertBand,
                          uint16_t,
                          AlertDirection,
                          VoiceAlertMode,
                          bool,
                          uint8_t) {}
void play_direction_only(AlertDirection, uint8_t) {}
void play_threat_escalation(AlertBand, uint16_t, AlertDirection, uint8_t, uint8_t, uint8_t, uint8_t) {}

static DisplayMode displayMode = DisplayMode::IDLE;
static V1Display display;
static PacketParser parser;
static V1BLEClient ble;
static AlertPersistenceModule alertPersistence;
static QuietCoordinatorModule quiet;
static SpeedMuteModule speedMute;
static VoiceModule voice;
static DisplayPipelineModule module;
static AlpRuntimeModule alpModule;
static AlpEventLatch alpLatch;

static AlertData makeKAlert(uint16_t freq = 24148) {
    AlertData a{};
    a.isValid = true;
    a.isPriority = true;
    a.band = BAND_K;
    a.frequency = freq;
    a.direction = DIR_FRONT;
    a.frontStrength = 5;
    return a;
}

static AlertData makeKaAlert(uint16_t freq = 34520) {
    AlertData a{};
    a.isValid = true;
    a.band = BAND_KA;
    a.frequency = freq;
    a.direction = DIR_REAR;
    a.rearStrength = 4;
    return a;
}

static void configureAlpActiveWithGun(AlpGunType gun,
                                      AlpLaserDirection direction);

static void enableSpeedMute(uint8_t targetVolume) {
    speedMute.begin(true, 25, 3, targetVolume);
    speedMute.update(10.0f, true, 2000);
}

static void beginModule() {
    DisplayPipelineDependencies dependencies;
    dependencies.displayMode = &displayMode;
    dependencies.display = &display;
    dependencies.parser = &parser;
    dependencies.settings = &settingsManager;
    dependencies.ble = &ble;
    dependencies.alertPersistence = &alertPersistence;
    dependencies.voice = &voice;
    dependencies.speedMute = &speedMute;
    dependencies.quiet = &quiet;
    dependencies.alp = &alpModule;
    dependencies.alpLatch = &alpLatch;
    module.begin(dependencies);
}

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
    display.reset();
    parser.reset();
    ble.reset();
    alertPersistence = AlertPersistenceModule{};
    speedMute = SpeedMuteModule{};
    voice = VoiceModule{};
    module = DisplayPipelineModule{};
    alpModule = AlpRuntimeModule{};
    alpLatch = AlpEventLatch{};
    displayMode = DisplayMode::IDLE;
    settingsManager = SettingsManager{};
    perfCounters.reset();
    perfExtended.reset();
    g_displayLogCalls = 0;
    g_lastDisplayLogEvent[0] = '\0';
    g_lastDisplayLogDetail[0] = '\0';
    quiet.begin(&ble, &parser);
    beginModule();
}

void tearDown() {}

void test_handle_parsed_updates_live_display_when_alert_present() {
    parser.state.signalBars = 1;  // Parsed local display bars.
    parser.setAlerts({makeKAlert()});

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(0, display.updatePersistedCalls);
    TEST_ASSERT_EQUAL(0, display.lastAlertUpdateCount);
    TEST_ASSERT_TRUE(display.hasLastAlertDisplayState);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, display.lastAlertDisplayState.signalBars,
        "main live bars should use the parsed local display strength");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, display.lastRenderFrame.context.signalBars,
        "parsed local display bars remain available as frame context");
}

void test_handle_parsed_updates_resting_display_when_idle() {
    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);

    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(0, display.updatePersistedCalls);
}

void test_handle_parsed_prefers_persisted_alert_when_configured() {
    settingsManager.slotAlertPersistSec[0] = 2;
    // Establish the active persistence slot before preloading a previously
    // live alert; production clears persistence on slot changes.
    module.handleParsed(900);
    display.reset();
    alertPersistence.setPersistedAlert(makeKAlert());

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);

    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(0, display.updateCalls);
    TEST_ASSERT_EQUAL(1, display.updatePersistedCalls);
}

void test_first_post_disconnect_display_frame_is_idle_not_stale_v1() {
    settingsManager.slotAlertPersistSec[0] = 2;
    parser.setAlerts({makeKAlert(24150)});
    alertPersistence.setPersistedAlert(makeKAlert(24150));
    alertPersistence.startPersistence(900);

    module.handleParsed(1000);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, display.lastRenderFrame.primaryKind);

    parser.resetAlertState();
    alertPersistence.clearPersistence();
    display.reset();

    module.handleParsed(1100);

    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_NOT_EQUAL(RenderFramePrimaryKind::V1_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_NOT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(0, display.updatePersistedCalls);
    TEST_ASSERT_FALSE(display.hasLastPriorityAlert);
    TEST_ASSERT_EQUAL(0, display.lastAlertUpdateCount);
}

void test_handle_parsed_defers_secondary_cards_while_connect_burst_settles() {
    ble.setConnectBurstSettling(true);
    parser.setAlerts({makeKAlert(), makeKaAlert()});

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(0, display.lastRenderFrame.cardCount);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(0, display.lastAlertUpdateCount);
}

void test_restore_current_owner_shows_scanning_when_ble_is_disconnected() {
    ble.setConnected(false);

    mockMillis = 2000;
    mockMicros = 2000 * 1000UL;
    module.restoreCurrentOwner(2000);

    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(1, display.showScanningCalls);
}

void test_restore_current_owner_ble_disconnected_restores_alp_live_when_active() {
    ble.setConnected(false);
    configureAlpActiveWithGun(AlpGunType::MARKSMAN_ULTRALYTE,
                              AlpLaserDirection::FRONT);

    mockMillis = 2000;
    mockMicros = 2000 * 1000UL;
    module.restoreCurrentOwner(2000);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(0, display.showScanningCalls);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(1, display.forceNextRedrawCalls);
    TEST_ASSERT_TRUE(display.lastAlpLaserEvent.active);
}

void test_restore_current_owner_restores_live_display_when_alerts_present() {
    parser.setAlerts({makeKAlert(24210)});

    mockMillis = 3000;
    mockMicros = 3000 * 1000UL;
    module.restoreCurrentOwner(3000);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(1, display.forceNextRedrawCalls);
}

// ── ALP synthetic alert path ──────────────────────────────────────────
//
// When ALP reports an active laser event, the composer promotes it to the
// primary frame and V1Display renders a synthesized BAND_LASER priority. If
// V1 radar is present at the same time, radar remains in the frame's
// secondary alert context, but ALP still owns the main laser
// presentation.
//
// These tests cover the ALP-primary frame branches in handleParsed() /
// restoreCurrentOwner().

static void configureAlpActiveWithGun(AlpGunType gun,
                                      AlpLaserDirection direction = AlpLaserDirection::UNKNOWN) {
    alpModule.testSetEnabled(true);
    alpModule.testSetState(AlpState::ALERT_ACTIVE);
    alpModule.testOpenSession(gun, /*isWarmUp=*/false, direction);
}

void test_handle_parsed_synthesizes_laser_alert_when_alp_active_and_no_v1() {
    configureAlpActiveWithGun(AlpGunType::MARKSMAN_ULTRALYTE,
                              AlpLaserDirection::FRONT);
    // No V1 alerts — pipeline must synthesize one from ALP state

    mockMillis = 2000;
    mockMicros = 2000 * 1000UL;
    module.handleParsed(2000);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(0, display.lastRenderFrame.cardCount);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(0, display.lastAlertUpdateCount);
    TEST_ASSERT_TRUE(display.hasLastPriorityAlert);
    TEST_ASSERT_TRUE(display.hasLastAlertDisplayState);

    const AlertData& a = display.lastPriorityAlert;
    TEST_ASSERT_TRUE(a.isValid);
    TEST_ASSERT_EQUAL(BAND_LASER, a.band);
    TEST_ASSERT_EQUAL(0, a.frequency);
    TEST_ASSERT_EQUAL(DIR_FRONT, a.direction);
    TEST_ASSERT_EQUAL(6, a.frontStrength);

    TEST_ASSERT_EQUAL(BAND_LASER, display.lastAlertDisplayState.activeBands);
    TEST_ASSERT_EQUAL(DIR_FRONT, display.lastAlertDisplayState.arrows);
    TEST_ASSERT_EQUAL(DIR_FRONT, display.lastAlertDisplayState.priorityArrow);
    TEST_ASSERT_EQUAL(8, display.lastAlertDisplayState.signalBars);  // laser = full local meter
    // Phase 2: setAlpLaserEvent should be called for the live ALP projection.
    TEST_ASSERT_EQUAL(1, display.setAlpLaserEventCalls);
    TEST_ASSERT_TRUE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, display.lastAlpLaserEvent.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::FRONT, display.lastAlpLaserEvent.direction);
}

void test_handle_parsed_keeps_unknown_alp_direction_off_screen() {
    configureAlpActiveWithGun(AlpGunType::MARKSMAN_ULTRALYTE,
                              AlpLaserDirection::UNKNOWN);

    mockMillis = 2050;
    mockMicros = 2050 * 1000UL;
    module.handleParsed(2050);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_TRUE(display.hasLastPriorityAlert);
    TEST_ASSERT_TRUE(display.hasLastAlertDisplayState);

    TEST_ASSERT_EQUAL(BAND_LASER, display.lastPriorityAlert.band);
    TEST_ASSERT_EQUAL(DIR_NONE, display.lastPriorityAlert.direction);
    TEST_ASSERT_EQUAL(BAND_LASER, display.lastAlertDisplayState.activeBands);
    TEST_ASSERT_EQUAL(DIR_NONE, display.lastAlertDisplayState.arrows);
    TEST_ASSERT_EQUAL(DIR_NONE, display.lastAlertDisplayState.priorityArrow);
    TEST_ASSERT_EQUAL(8, display.lastAlertDisplayState.signalBars);  // laser = full local meter
    TEST_ASSERT_EQUAL(1, display.setAlpLaserEventCalls);
    TEST_ASSERT_TRUE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(AlpLaserDirection::UNKNOWN, display.lastAlpLaserEvent.direction);
    TEST_ASSERT_NOT_NULL(strstr(g_lastDisplayLogDetail, "dp=LASER/0/NONE"));
    TEST_ASSERT_NOT_NULL(strstr(g_lastDisplayLogDetail, "d=UNKNOWN"));
}

void test_handle_parsed_prioritizes_alp_laser_over_v1_radar() {
    configureAlpActiveWithGun(AlpGunType::MARKSMAN_ULTRALYTE,
                              AlpLaserDirection::FRONT);
    parser.setAlerts({makeKAlert(24210), makeKaAlert(34520)});

    mockMillis = 2100;
    mockMicros = 2100 * 1000UL;
    module.handleParsed(2100);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(2, display.lastRenderFrame.cardCount);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(2, display.lastAlertUpdateCount);
    TEST_ASSERT_TRUE(display.hasLastPriorityAlert);
    TEST_ASSERT_TRUE(display.hasLastAlertDisplayState);

    const AlertData& a = display.lastPriorityAlert;
    TEST_ASSERT_TRUE(a.isValid);
    TEST_ASSERT_EQUAL(BAND_LASER, a.band);
    TEST_ASSERT_EQUAL(0, a.frequency);
    TEST_ASSERT_EQUAL(DIR_FRONT, a.direction);
    TEST_ASSERT_EQUAL(6, a.frontStrength);

    TEST_ASSERT_EQUAL(BAND_LASER, display.lastAlertDisplayState.activeBands);
    TEST_ASSERT_EQUAL(DIR_FRONT, display.lastAlertDisplayState.arrows);
    TEST_ASSERT_EQUAL(DIR_FRONT, display.lastAlertDisplayState.priorityArrow);
    TEST_ASSERT_EQUAL(8, display.lastAlertDisplayState.signalBars);  // laser = full local meter
    TEST_ASSERT_TRUE(g_displayLogCalls > 0);
    TEST_ASSERT_EQUAL_STRING("DISP_LIVE", g_lastDisplayLogEvent);
    TEST_ASSERT_NOT_NULL(strstr(g_lastDisplayLogDetail, "g=ULT"));
    TEST_ASSERT_NOT_NULL(strstr(g_lastDisplayLogDetail, "v1c=2"));
    TEST_ASSERT_NOT_NULL(strstr(g_lastDisplayLogDetail, "dp=LASER/0/FRONT"));
    TEST_ASSERT_NOT_NULL(strstr(g_lastDisplayLogDetail, "rc=2"));
}

void test_handle_parsed_keeps_v1_cards_when_alp_is_primary() {
    configureAlpActiveWithGun(AlpGunType::MARKSMAN_ULTRALYTE,
                              AlpLaserDirection::FRONT);
    parser.setAlerts({makeKAlert(24210)});

    mockMillis = 2150;
    mockMicros = 2150 * 1000UL;
    module.handleParsed(2150);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(1, display.lastRenderFrame.cardCount);
    TEST_ASSERT_EQUAL(RenderFrameCard::Kind::V1, display.lastRenderFrame.cards[0].kind);
    TEST_ASSERT_TRUE(display.lastRenderFrame.cards[0].v1Alert.isValid);
    TEST_ASSERT_EQUAL(BAND_K, display.lastRenderFrame.cards[0].v1Alert.band);
    TEST_ASSERT_EQUAL(24210, display.lastRenderFrame.cards[0].v1Alert.frequency);
    TEST_ASSERT_EQUAL(DIR_FRONT, display.lastRenderFrame.cards[0].v1Alert.direction);
}

void test_handle_parsed_does_not_synthesize_when_alp_inactive() {
    // ALP wired but no session open — hasLaserEvent() == false
    alpModule.testSetEnabled(true);
    alpModule.testSetState(AlpState::LISTENING);
    // No V1 alerts, no ALP event

    mockMillis = 2000;
    mockMicros = 2000 * 1000UL;
    module.handleParsed(2000);

    // Should fall through to idle, NOT render a synthetic laser
    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(0, display.lastAlertUpdateCount);
}

void test_handle_parsed_suppresses_synthetic_alert_during_alp_warm_up() {
    alpModule.testSetEnabled(true);
    alpModule.testSetState(AlpState::ALERT_ACTIVE);
    // Warm-Up session → hasLaserEvent() returns false
    alpModule.testOpenSession(AlpGunType::UNKNOWN, /*isWarmUp=*/true);

    mockMillis = 2000;
    mockMicros = 2000 * 1000UL;
    module.handleParsed(2000);

    // During Warm-Up suppression the display must stay clean
    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(0, display.lastAlertUpdateCount);
}

void test_handle_parsed_clears_alp_projection_during_teardown_gap() {
    // TEARDOWN without a gun ID is a phantom session — display must clear.
    // (TEARDOWN *with* a gun keeps the display alive; see updateCurrentEvent.)
    alpModule.testSetEnabled(true);
    alpModule.testSetState(AlpState::TEARDOWN);
    alpModule.testOpenSession(AlpGunType::UNKNOWN, /*isWarmUp=*/false,
                              AlpLaserDirection::REAR);

    mockMillis = 2500;
    mockMicros = 2500 * 1000UL;
    module.handleParsed(2500);

    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(0, display.lastAlertUpdateCount);
    // Phase 2: setAlpLaserEvent should be called (replaces clear/setDirection)
    TEST_ASSERT_EQUAL(1, display.setAlpLaserEventCalls);
    TEST_ASSERT_FALSE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL_STRING("DISP_IDLE", g_lastDisplayLogEvent);
    TEST_ASSERT_NOT_NULL(strstr(g_lastDisplayLogDetail, "own=1"));
    TEST_ASSERT_NOT_NULL(strstr(g_lastDisplayLogDetail, "dp=NONE"));
}

void test_handle_parsed_keeps_alp_alert_live_until_normal_listening_heartbeat_returns() {
    settingsManager.alpAlertPersistSec = 0;
    configureAlpActiveWithGun(AlpGunType::PL3_PROLITE,
                              AlpLaserDirection::FRONT);

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_TRUE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, display.lastAlpLaserEvent.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::FRONT, display.lastAlpLaserEvent.direction);

    // Parser/session truth says the event is closed, but the heartbeat has
    // not yet settled back to a normal LISTENING mode. Keep showing the last
    // alert context until that normal heartbeat returns.
    alpModule.testSetLastHbByte1(0x00);
    alpModule.testSetState(AlpState::LISTENING, 1000);
    alpModule.testCloseSession(1000);

    display.reset();
    mockMillis = 1100;
    mockMicros = 1100 * 1000UL;
    module.handleParsed(1100);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_TRUE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, display.lastAlpLaserEvent.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::FRONT, display.lastAlpLaserEvent.direction);

    // Once a normal LISTENING heartbeat returns, the display-side alert latch
    // releases and the screen falls back to idle/persist behavior.
    alpModule.testSetLastHbByte1(0x03);

    display.reset();
    mockMillis = 1200;
    mockMicros = 1200 * 1000UL;
    module.handleParsed(1200);

    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_FALSE(display.lastAlpLaserEvent.active);
}

void test_handle_parsed_clears_stale_alp_presentation_after_listening_hold_dwell() {
    settingsManager.alpAlertPersistSec = 0;
    configureAlpActiveWithGun(AlpGunType::MARKSMAN_ULTRALYTE,
                              AlpLaserDirection::REAR);

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_TRUE(display.lastAlpLaserEvent.active);

    // After the encounter closes, a brief abnormal LISTENING heartbeat may
    // still keep the display context alive, but only for a bounded dwell.
    alpModule.testSetLastHbByte1(0x00);
    alpModule.testSetState(AlpState::LISTENING, 1000);
    alpModule.testCloseSession(1000);

    display.reset();
    mockMillis = 1500;
    mockMicros = 1500 * 1000UL;
    module.handleParsed(1500);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_TRUE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, display.lastAlpLaserEvent.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::REAR, display.lastAlpLaserEvent.direction);

    display.reset();
    mockMillis = 2501;
    mockMicros = 2501 * 1000UL;
    module.handleParsed(2501);

    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_FALSE(display.lastAlpLaserEvent.active);
}

void test_handle_parsed_preserves_best_known_alp_context_during_live_unknown_updates() {
    configureAlpActiveWithGun(AlpGunType::PL3_PROLITE,
                              AlpLaserDirection::FRONT);

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, display.lastAlpLaserEvent.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::FRONT, display.lastAlpLaserEvent.direction);

    alpModule.testOpenSession(AlpGunType::UNKNOWN, /*isWarmUp=*/false,
                              AlpLaserDirection::UNKNOWN);

    display.reset();
    mockMillis = 1100;
    mockMicros = 1100 * 1000UL;
    module.handleParsed(1100);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_TRUE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, display.lastAlpLaserEvent.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::FRONT, display.lastAlpLaserEvent.direction);
}

void test_handle_parsed_keeps_prior_alp_context_through_teardown_only_rearm_gap() {
    settingsManager.alpAlertPersistSec = 0;
    configureAlpActiveWithGun(AlpGunType::PL3_PROLITE,
                              AlpLaserDirection::FRONT);

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_TRUE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, display.lastAlpLaserEvent.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::FRONT, display.lastAlpLaserEvent.direction);

    // The runtime times out to LISTENING while the ALP is still toggling
    // Targeted/idle heartbeats for the same physical encounter.
    alpModule.testSetLastHbByte1(0x01);
    alpModule.testSetState(AlpState::LISTENING, 1000);
    alpModule.testCloseSession(1000);

    display.reset();
    mockMillis = 1100;
    mockMicros = 1100 * 1000UL;
    module.handleParsed(1100);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_TRUE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, display.lastAlpLaserEvent.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::FRONT, display.lastAlpLaserEvent.direction);

    // Same encounter immediately reopens and falls back into TEARDOWN
    // before a new gun frame arrives. Keep showing the prior context.
    alpModule.testOpenSession(AlpGunType::UNKNOWN, /*isWarmUp=*/false,
                              AlpLaserDirection::FRONT);
    alpModule.testSetState(AlpState::TEARDOWN);
    alpModule.testSetLastHbByte1(0x00);

    display.reset();
    mockMillis = 1200;
    mockMicros = 1200 * 1000UL;
    module.handleParsed(1200);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_TRUE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, display.lastAlpLaserEvent.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::FRONT, display.lastAlpLaserEvent.direction);
}

void test_restore_current_owner_synthesizes_laser_alert_when_alp_active() {
    configureAlpActiveWithGun(AlpGunType::PL3_PROLITE,
                              AlpLaserDirection::REAR);
    // No V1 alerts on restore — the alpActive branch must fire here too

    mockMillis = 3000;
    mockMicros = 3000 * 1000UL;
    module.restoreCurrentOwner(3000);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(0, display.lastRenderFrame.cardCount);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_TRUE(display.hasLastPriorityAlert);
    TEST_ASSERT_TRUE(display.hasLastAlertDisplayState);
    TEST_ASSERT_EQUAL(BAND_LASER, display.lastPriorityAlert.band);
    TEST_ASSERT_EQUAL(DIR_REAR, display.lastPriorityAlert.direction);
    TEST_ASSERT_EQUAL(6, display.lastPriorityAlert.frontStrength);
    TEST_ASSERT_EQUAL(BAND_LASER, display.lastAlertDisplayState.activeBands);
    TEST_ASSERT_EQUAL(DIR_REAR, display.lastAlertDisplayState.arrows);
    TEST_ASSERT_EQUAL(DIR_REAR, display.lastAlertDisplayState.priorityArrow);
    TEST_ASSERT_EQUAL(8, display.lastAlertDisplayState.signalBars);  // laser = full local meter
    // Phase 2: setAlpLaserEvent should be called
    TEST_ASSERT_EQUAL(1, display.setAlpLaserEventCalls);
    TEST_ASSERT_TRUE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, display.lastAlpLaserEvent.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::REAR, display.lastAlpLaserEvent.direction);
}

void test_restore_current_owner_prioritizes_alp_laser_over_v1_radar() {
    configureAlpActiveWithGun(AlpGunType::PL3_PROLITE,
                              AlpLaserDirection::REAR);
    parser.setAlerts({makeKAlert(24210), makeKaAlert(34520)});

    mockMillis = 3100;
    mockMicros = 3100 * 1000UL;
    module.restoreCurrentOwner(3100);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(2, display.lastRenderFrame.cardCount);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_TRUE(display.hasLastPriorityAlert);
    TEST_ASSERT_TRUE(display.hasLastAlertDisplayState);
    TEST_ASSERT_EQUAL(2, display.lastAlertUpdateCount);
    TEST_ASSERT_EQUAL(BAND_LASER, display.lastPriorityAlert.band);
    TEST_ASSERT_EQUAL(DIR_REAR, display.lastPriorityAlert.direction);
    TEST_ASSERT_EQUAL(6, display.lastPriorityAlert.frontStrength);
    TEST_ASSERT_EQUAL(BAND_LASER, display.lastAlertDisplayState.activeBands);
    TEST_ASSERT_EQUAL(DIR_REAR, display.lastAlertDisplayState.arrows);
    TEST_ASSERT_EQUAL(DIR_REAR, display.lastAlertDisplayState.priorityArrow);
    TEST_ASSERT_EQUAL(8, display.lastAlertDisplayState.signalBars);  // laser = full local meter
}

void test_handle_parsed_keeps_latched_alp_event_within_persist_window() {
    // ALP uses its own persist window — not V1's per-slot alertPersistSec.
    settingsManager.alpAlertPersistSec = 2;
    configureAlpActiveWithGun(AlpGunType::PL3_PROLITE,
                              AlpLaserDirection::REAR);

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);

    alpModule.testSetState(AlpState::TEARDOWN);
    alpModule.testCloseSession();

    display.reset();
    mockMillis = 1500;
    mockMicros = 1500 * 1000UL;
    module.handleParsed(1500);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_PERSISTED, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_FALSE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, display.lastAlpLaserEvent.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::REAR, display.lastAlpLaserEvent.direction);
    TEST_ASSERT_EQUAL(BAND_LASER, display.lastPriorityAlert.band);
}

void test_handle_parsed_clears_latched_alp_event_after_persist_window() {
    settingsManager.alpAlertPersistSec = 2;
    configureAlpActiveWithGun(AlpGunType::PL3_PROLITE,
                              AlpLaserDirection::REAR);

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);

    alpModule.testSetState(AlpState::TEARDOWN);
    alpModule.testCloseSession();

    mockMillis = 1500;
    mockMicros = 1500 * 1000UL;
    module.handleParsed(1500);

    display.reset();
    mockMillis = 3500;
    mockMicros = 3500 * 1000UL;
    module.handleParsed(3500);

    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_FALSE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(0, display.lastAlertUpdateCount);
}

void test_handle_parsed_does_not_restart_alp_persistence_window() {
    settingsManager.alpAlertPersistSec = 2;
    configureAlpActiveWithGun(AlpGunType::MARKSMAN_ULTRALYTE,
                              AlpLaserDirection::FRONT);

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);

    alpModule.testSetState(AlpState::TEARDOWN);
    alpModule.testCloseSession();

    mockMillis = 1500;
    mockMicros = 1500 * 1000UL;
    module.handleParsed(1500);

    display.reset();
    mockMillis = 3200;
    mockMicros = 3200 * 1000UL;
    module.handleParsed(3200);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_PERSISTED, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_FALSE(display.lastAlpLaserEvent.active);

    display.reset();
    mockMillis = 3501;
    mockMicros = 3501 * 1000UL;
    module.handleParsed(3501);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_FALSE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
}

void test_handle_parsed_keeps_latched_alp_event_after_live_ownership_drops() {
    settingsManager.alpAlertPersistSec = 5;
    configureAlpActiveWithGun(AlpGunType::PL3_PROLITE,
                              AlpLaserDirection::REAR);

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);

    alpModule.testSetState(AlpState::TEARDOWN);
    alpModule.testCloseSession();

    mockMillis = 1500;
    mockMicros = 1500 * 1000UL;
    module.handleParsed(1500);

    display.reset();
    alpModule.testSetState(AlpState::IDLE);  // simulate heartbeat timeout dropping live ownership
    mockMillis = 4500;
    mockMicros = 4500 * 1000UL;
    module.handleParsed(4500);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_PERSISTED, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_FALSE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, display.lastAlpLaserEvent.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::REAR, display.lastAlpLaserEvent.direction);
}

void test_handle_parsed_swaps_to_new_live_alp_event_when_new_event_arrives_mid_persist() {
    settingsManager.alpAlertPersistSec = 5;
    configureAlpActiveWithGun(AlpGunType::PL3_PROLITE,
                              AlpLaserDirection::REAR);

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);

    alpModule.testSetState(AlpState::TEARDOWN);
    alpModule.testCloseSession();

    display.reset();
    mockMillis = 1500;
    mockMicros = 1500 * 1000UL;
    module.handleParsed(1500);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_PERSISTED, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_FALSE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, display.lastAlpLaserEvent.gun);

    configureAlpActiveWithGun(AlpGunType::MARKSMAN_ULTRALYTE,
                              AlpLaserDirection::FRONT);

    display.reset();
    mockMillis = 1800;
    mockMicros = 1800 * 1000UL;
    module.handleParsed(1800);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.renderFrameCalls);
    TEST_ASSERT_TRUE(display.hasLastRenderFrame);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_TRUE(display.lastAlpLaserEvent.active);
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, display.lastAlpLaserEvent.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::FRONT, display.lastAlpLaserEvent.direction);
    TEST_ASSERT_EQUAL(BAND_LASER, display.lastPriorityAlert.band);
    TEST_ASSERT_EQUAL(DIR_FRONT, display.lastPriorityAlert.direction);
}

void test_handle_parsed_drops_alp_immediately_when_persist_disabled() {
    // Regression for on-device bug: with alpAlertPersistSec=0 (default), the
    // latch must release the moment ALP reports active=0 instead of holding
    // a phantom laser for the slot's V1 alertPersistSec. Log evidence: after
    // a real PL3 DLI engagement, SESSION_DISPLAY_WINDOW_CLOSE at t=23000 was
    // followed by DISP_V1_EVENT active=0 3s later because the ALP was
    // reading the V1 persist window (2s). With the dedicated ALP setting at
    // 0 the display must drop to IDLE the first tick after close.
    settingsManager.alpAlertPersistSec = 0;
    settingsManager.slotAlertPersistSec[0] = 5;  // V1 window is long; ALP must ignore it
    configureAlpActiveWithGun(AlpGunType::PL3_PROLITE,
                              AlpLaserDirection::REAR);

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, display.lastRenderFrame.primaryKind);

    // Engagement ends — session closes.
    alpModule.testSetState(AlpState::TEARDOWN);
    alpModule.testCloseSession();

    display.reset();
    mockMillis = 1100;
    mockMicros = 1100 * 1000UL;
    module.handleParsed(1100);

    // Must be IDLE immediately — no ALP_PERSISTED tail.
    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_FALSE(display.lastAlpLaserEvent.active);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handle_parsed_updates_live_display_when_alert_present);
    RUN_TEST(test_handle_parsed_updates_resting_display_when_idle);
    RUN_TEST(test_handle_parsed_prefers_persisted_alert_when_configured);
    RUN_TEST(test_first_post_disconnect_display_frame_is_idle_not_stale_v1);
    RUN_TEST(test_handle_parsed_defers_secondary_cards_while_connect_burst_settles);
    RUN_TEST(test_restore_current_owner_shows_scanning_when_ble_is_disconnected);
    RUN_TEST(test_restore_current_owner_ble_disconnected_restores_alp_live_when_active);
    RUN_TEST(test_restore_current_owner_restores_live_display_when_alerts_present);
    RUN_TEST(test_handle_parsed_synthesizes_laser_alert_when_alp_active_and_no_v1);
    RUN_TEST(test_handle_parsed_keeps_unknown_alp_direction_off_screen);
    RUN_TEST(test_handle_parsed_prioritizes_alp_laser_over_v1_radar);
    RUN_TEST(test_handle_parsed_keeps_v1_cards_when_alp_is_primary);
    RUN_TEST(test_handle_parsed_does_not_synthesize_when_alp_inactive);
    RUN_TEST(test_handle_parsed_suppresses_synthetic_alert_during_alp_warm_up);
    RUN_TEST(test_handle_parsed_clears_alp_projection_during_teardown_gap);
    RUN_TEST(test_handle_parsed_keeps_alp_alert_live_until_normal_listening_heartbeat_returns);
    RUN_TEST(test_handle_parsed_clears_stale_alp_presentation_after_listening_hold_dwell);
    RUN_TEST(test_handle_parsed_preserves_best_known_alp_context_during_live_unknown_updates);
    RUN_TEST(test_handle_parsed_keeps_prior_alp_context_through_teardown_only_rearm_gap);
    RUN_TEST(test_restore_current_owner_synthesizes_laser_alert_when_alp_active);
    RUN_TEST(test_restore_current_owner_prioritizes_alp_laser_over_v1_radar);
    RUN_TEST(test_handle_parsed_keeps_latched_alp_event_within_persist_window);
    RUN_TEST(test_handle_parsed_clears_latched_alp_event_after_persist_window);
    RUN_TEST(test_handle_parsed_does_not_restart_alp_persistence_window);
    RUN_TEST(test_handle_parsed_keeps_latched_alp_event_after_live_ownership_drops);
    RUN_TEST(test_handle_parsed_swaps_to_new_live_alp_event_when_new_event_arrives_mid_persist);
    RUN_TEST(test_handle_parsed_drops_alp_immediately_when_persist_disabled);
    return UNITY_END();
}
