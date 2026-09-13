#include <unity.h>

#include <ArduinoJson.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#ifndef CONFIG_H
#define CONFIG_H
#define ESP_PACKET_START 0xAA
#define ESP_PACKET_END 0xAB
#define PACKET_ID_DISPLAY_DATA 0x31
#define PACKET_ID_ALERT_DATA 0x43
#define PACKET_ID_WRITE_USER_BYTES 0x13
#define PACKET_ID_TURN_OFF_DISPLAY 0x32
#define PACKET_ID_TURN_ON_DISPLAY 0x33
#define PACKET_ID_MUTE_ON 0x34
#define PACKET_ID_MUTE_OFF 0x35
#define PACKET_ID_REQ_CURRENT_VOLUME 0x37
#define PACKET_ID_RESP_CURRENT_VOLUME 0x38
#define PACKET_ID_REQ_WRITE_VOLUME 0x39
#define PACKET_ID_RESP_USER_BYTES 0x12
#define PACKET_ID_VERSION 0x01
#define PACKET_ID_RESP_VERSION 0x02
#define PACKET_ID_REQ_ALL_VOLUME 0x3C
#define PACKET_ID_RESP_ALL_VOLUME 0x3D
#endif

#include "../mocks/ble_client.h"
#include "../mocks/display.h"
#include "../mocks/settings.h"
#include "../mocks/v1_profiles.h"
#include "../mocks/modules/speed_mute/speed_mute_module.h"
#include "../mocks/modules/volume_fade/volume_fade_module.h"

#include "../../src/packet_parser.h"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_templates.h"
#include "../../src/modules/auto_push/auto_push_module.cpp"

#include <array>
#include <vector>

static V1BLEClient ble;
static V1Display display;
static PacketParser parser;
static SettingsManager settings;
static V1ProfileManager profiles;
static QuietCoordinatorModule quiet;
static AutoPushModule module;

namespace {

std::vector<uint8_t> makeV1Packet(uint8_t id, const std::vector<uint8_t>& data,
                                  uint8_t origin = 0xEA, uint8_t destination = 0) {
    if (destination == 0) destination = id == PACKET_ID_DISPLAY_DATA ? 0xD8 : 0xD6;
    std::vector<uint8_t> packet{0xAA, destination, origin, id,
                                static_cast<uint8_t>(data.size() + (origin == 0xEA ? 1 : 0))};
    packet.insert(packet.end(), data.begin(), data.end());
    if (origin == 0xEA) {
        uint8_t checksum = 0;
        for (uint8_t value : packet) checksum = static_cast<uint8_t>(checksum + value);
        packet.push_back(checksum);
    }
    packet.push_back(0xAB);
    return packet;
}

void parseVersion(uint32_t version = 41039) {
    const uint8_t major = static_cast<uint8_t>((version / 10000) % 10);
    const uint8_t minor = static_cast<uint8_t>((version / 1000) % 10);
    const uint8_t r1 = static_cast<uint8_t>((version / 100) % 10);
    const uint8_t r2 = static_cast<uint8_t>((version / 10) % 10);
    const uint8_t control = static_cast<uint8_t>(version % 10);
    const auto packet = makeV1Packet(PACKET_ID_RESP_VERSION,
                                     {'v', static_cast<uint8_t>('0' + major), '.', static_cast<uint8_t>('0' + minor),
                                      static_cast<uint8_t>('0' + r1), static_cast<uint8_t>('0' + r2),
                                      static_cast<uint8_t>('0' + control)});
    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), mockMillis));
    ble.onV1FirmwareVersionReceived(version);
}

void observeDisplay(bool on, uint8_t mode, uint8_t origin = 0xEA, bool corruptChecksum = false,
                    uint8_t destination = 0xD8, uint32_t ingressSequence = UINT32_MAX) {
    const uint8_t aux0 = static_cast<uint8_t>(0x04 | (on ? 0x08 : 0x00));
    auto packet = makeV1Packet(PACKET_ID_DISPLAY_DATA,
                               {0x3F, 0x3F, 0x00, 0x00, 0x00, aux0,
                                static_cast<uint8_t>((mode & 0x03) << 2), 0x52}, origin, destination);
    if (corruptChecksum && origin == 0xEA) packet[packet.size() - 2] ^= 0x01;
    if (ingressSequence == UINT32_MAX) ingressSequence = ble.noteV1NotificationIngress();
    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), mockMillis, ingressSequence));
}

void observeCurrentVolume(uint8_t main, uint8_t muted, uint8_t origin = 0xEA,
                          bool corruptChecksum = false, uint8_t destination = 0xD6,
                          uint32_t ingressSequence = UINT32_MAX) {
    auto packet = makeV1Packet(PACKET_ID_RESP_CURRENT_VOLUME, {main, muted}, origin, destination);
    if (corruptChecksum && origin == 0xEA) packet[packet.size() - 2] ^= 0x01;
    if (ingressSequence == UINT32_MAX) ingressSequence = ble.noteV1NotificationIngress();
    const bool parsed = parser.parse(packet.data(), packet.size(), mockMillis, ingressSequence);
    TEST_ASSERT_EQUAL(!corruptChecksum, parsed);
}

void observeAllVolume(uint8_t main, uint8_t muted, uint8_t savedMain, uint8_t savedMuted) {
    const auto packet = makeV1Packet(PACKET_ID_RESP_ALL_VOLUME, {main, muted, savedMain, savedMuted});
    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), mockMillis,
                                  ble.noteV1NotificationIngress()));
}

void observeUserBytes(const uint8_t* bytes, uint32_t ingressSequence = UINT32_MAX) {
    if (ingressSequence == UINT32_MAX) ingressSequence = ble.noteV1NotificationIngress();
    ble.onUserBytesReceived(bytes, ingressSequence);
}

void injectMatchingUserBytesDuringSend() { observeUserBytes(ble.lastUserBytes); }
void injectMatchingDisplayDuringSend() { observeDisplay(ble.lastDisplayOnValue, 1); }
void injectMatchingModeDuringSend() { observeDisplay(true, ble.lastModeValue); }
void injectMatchingVolumeDuringSend() { observeCurrentVolume(ble.lastVolume, ble.lastMuteVolume); }

bool statusContains(const char* text) {
    return module.getStatusJson().indexOf(text) >= 0;
}

void at(unsigned long now) {
    mockMillis = now;
    module.process();
}

void configureProfile(const std::array<uint8_t, 6>& desired = {{0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}) {
    settings.settings.autoPushProfileSchemaVersion = V1_PROFILE_SCHEMA_VERSION;
    settings.slotConfigs[0].profileName = "ROAD";
    profiles.loadProfileSuccess = true;
    profiles.nextLoadStatus = ProfileStorageStatus::NotFound;
    profiles.loadableProfileName = "ROAD";
    profiles.loadableProfile.name = "ROAD";
    profiles.loadableProfile.schemaVersion = V1_PROFILE_SCHEMA_VERSION;
    std::memcpy(profiles.loadableProfile.settings.bytes, desired.data(), desired.size());
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Value;
    detector.modePolicy = V1ModePolicy::Value;
    detector.mode = 2;
    detector.displayPolicy = V1DisplayPolicy::Off;
    detector.volumePolicy = V1VolumePolicy::Temporary;
    detector.mainVolume = 7;
    detector.mutedVolume = 3;
}

V1DetectorSnapshot makeSnapshot(uint32_t version = 41039,
                                const std::array<uint8_t, 6>& user = {{0xFF, 0xFF, 0xFF, 0xFF, 0xA4, 0x5A}}) {
    V1DetectorSnapshot snapshot;
    snapshot.available = true;
    snapshot.capturedUptimeMs = mockMillis;
    snapshot.sessionGeneration = ble.sessionGeneration();
    snapshot.hasFirmwareVersion = true;
    snapshot.firmwareVersion = version;
    snapshot.hasUserBytes = true;
    snapshot.userBytes = user;
    snapshot.hasDisplayOn = true;
    snapshot.displayOn = true;
    snapshot.hasMode = true;
    snapshot.mode = 'A';
    snapshot.hasCurrentVolume = true;
    snapshot.currentMainVolume = 5;
    snapshot.currentMutedVolume = 2;
    return snapshot;
}

void stageSnapshot(const V1DetectorSnapshot& snapshot, bool primeObservations = true) {
    parseVersion(snapshot.firmwareVersion);
    if (snapshot.hasUserBytes) observeUserBytes(snapshot.userBytes.data());
    if (primeObservations && snapshot.hasDisplayOn && snapshot.hasMode) {
        observeDisplay(snapshot.displayOn, snapshot.mode == 'L' ? 3 : snapshot.mode == 'l' ? 2 : 1);
    }
    if (primeObservations && snapshot.hasCurrentVolume) {
        observeCurrentVolume(snapshot.currentMainVolume, snapshot.currentMutedVolume);
    }
    module.setPreApplySnapshot(snapshot);
}

void queueAndPreflight() {
    TEST_ASSERT_EQUAL_INT(AutoPushModule::QueueResult::QUEUED, module.queueSlotPush(0));
    at(100); // WaitReady -> LoadProfile
    at(100); // load -> Preflight
    at(100); // whole-plan preflight -> first needed component (or terminal)
}

void verifyUserBytes(unsigned long now = 130) {
    at(100); // UserWrite
    at(130); // UserRead
    observeUserBytes(ble.lastUserBytes);
    at(now); // UserVerify
}

void finishFullApply() {
    at(160); // DisplayWrite
    observeDisplay(false, 1);
    at(160); // DisplayVerify
    at(190); // ModeWrite
    observeDisplay(false, 2);
    at(190); // ModeVerify
    at(220); // VolumeWrite
    at(250); // VolumeRead
    observeCurrentVolume(7, 3);
    at(250); // VolumeVerify
}

} // namespace

void setUp() {
    ble.reset();
    display = V1Display{};
    parser = PacketParser{};
    settings = SettingsManager{};
    profiles.reset();
    mockMillis = 0;
    mockMicros = 0;
    quiet.begin(&ble, &parser);
    module = AutoPushModule{};
    module.begin(&settings, &profiles, &ble, &parser, &display, &quiet);
}

void tearDown() {}

void test_full_apply_requires_fresh_canonical_readbacks_for_every_component() {
    configureProfile();
    stageSnapshot(makeSnapshot());
    ble.startUserBytesVerification(ble.sessionUserBytes);
    queueAndPreflight();
    verifyUserBytes();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, ble.cancelUserBytesVerificationCalls);
    TEST_ASSERT_FALSE(ble.consumeVerifyPushMatchEdge());
    finishFullApply();

    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_TRUE(statusContains("\"profile\":{\"requested\":true"));
    TEST_ASSERT_TRUE(statusContains("\"outcome\":\"verified\""));
    TEST_ASSERT_EQUAL_INT(1, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(1, ble.setDisplayOnCalls);
    TEST_ASSERT_EQUAL_INT(1, ble.setModeCalls);
    TEST_ASSERT_EQUAL_INT(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_INT(1, ble.requestCurrentVolumeCalls);
    JsonDocument status;
    const String statusJson = module.getStatusJson();
    TEST_ASSERT_FALSE(deserializeJson(status, statusJson.c_str()));
    TEST_ASSERT_EQUAL_STRING("succeeded", status["result"].as<const char*>());
    TEST_ASSERT_TRUE(ble.consumeVerifyPushMatchEdge());
    TEST_ASSERT_FALSE(ble.consumeVerifyPushMatchEdge());
}

void test_all_noop_components_are_proven_unchanged_without_writes() {
    const std::array<uint8_t, 6> bytes{{0xFF, 0xFF, 0xFF, 0xFF, 0xA4, 0x5A}};
    configureProfile(bytes);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::On;
    detector.mode = 1;
    detector.mainVolume = 5;
    detector.mutedVolume = 2;
    stageSnapshot(makeSnapshot(41039, bytes));
    queueAndPreflight();

    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_TRUE(statusContains("\"outcome\":\"unchanged\""));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setDisplayOnCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setModeCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
    TEST_ASSERT_TRUE(ble.consumeVerifyPushMatchEdge());
}

void test_supported_user_masks_match_every_vendor_boundary() {
    struct Case {
        uint32_t version;
        uint8_t expected[6];
    };
    const Case cases[] = {
        {41018, {0xFF, 0x0F, 0x00, 0x00, 0x00, 0x00}},
        {41030, {0xFF, 0x0F, 0x00, 0x00, 0x00, 0x00}},
        {41031, {0xFF, 0x3F, 0x00, 0x00, 0x00, 0x00}},
        {41032, {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00}},
        {41034, {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00}},
        {41035, {0xFF, 0xFF, 0x07, 0x00, 0x00, 0x00}},
        {41036, {0xFF, 0xFF, 0x1F, 0x00, 0x00, 0x00}},
        {41037, {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00}},
        {41038, {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00}},
        {41039, {0xFF, 0xFF, 0xFF, 0xFF, 0x03, 0x00}},
    };
    for (const auto& test : cases) {
        uint8_t actual[6] = {};
        V1FirmwareCompat::supportedUserByteMasks(test.version, actual);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(test.expected, actual, sizeof(actual));
    }
}

void test_supported_masks_preserve_unknown_bits_and_four_byte_firmware_shape() {
    const std::array<uint8_t, 6> before{{0xFF, 0xFF, 0xA0, 0x55, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> desired{{0xFE, 0xCF, 0x5F, 0xAA, 0x03, 0x00}};
    configureProfile(desired);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot(41038, before));
    queueAndPreflight();
    at(100);

    const uint8_t expected[] = {0xFE, 0xCF, 0x5F, 0xAA, 0xA4, 0x5A};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, ble.lastUserBytes, 6);
    TEST_ASSERT_TRUE(statusContains("\"supportedMasks\":[255,255,255,255,0,0]"));
}

void test_41039_masks_only_modeled_byte_four_bits_and_preserves_byte_five() {
    const std::array<uint8_t, 6> before{{0xFF, 0xFF, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> desired{{0xFE, 0xFF, 0xFF, 0xFF, 0x03, 0x00}};
    configureProfile(desired);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot(41039, before));
    queueAndPreflight();
    at(100);

    const uint8_t expected[] = {0xFE, 0xFF, 0xFF, 0xFF, 0xA7, 0x5A};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, ble.lastUserBytes, 6);
    TEST_ASSERT_TRUE(statusContains("\"supportedMasks\":[255,255,255,255,3,0]"));
}

void test_alp_laser_override_is_in_effective_bytes_and_verified() {
    const std::array<uint8_t, 6> before{{0xFF, 0xFF, 0xFF, 0xFF, 0xA4, 0x5A}};
    configureProfile(before);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    settings.settings.alpEnabled = true;
    settings.settings.alpDisableV1LaserOnPush = true;
    stageSnapshot(makeSnapshot(41039, before));
    queueAndPreflight();
    at(100);

    TEST_ASSERT_EQUAL_HEX8(0xF7, ble.lastUserBytes[0]);
    TEST_ASSERT_TRUE(statusContains("\"desired\":[255,255,255,255,164,90]"));
    TEST_ASSERT_TRUE(statusContains("\"effective\":[247,255,255,255,164,90]"));
    TEST_ASSERT_TRUE(statusContains("\"alpLaserOverrideApplied\":true"));
}

void test_user_readback_mismatch_is_failure_not_applied() {
    configureProfile();
    auto snapshot = makeSnapshot();
    profiles.loadableProfile.detector.displayPolicy = V1DisplayPolicy::Unchanged;
    profiles.loadableProfile.detector.modePolicy = V1ModePolicy::Unchanged;
    profiles.loadableProfile.detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(snapshot);
    queueAndPreflight();
    at(100);
    at(130);
    const uint8_t wrong[] = {0xFD, 0xFF, 0xFF, 0xFF, 0xA4, 0x5A};
    observeUserBytes(wrong);
    at(130);

    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
    TEST_ASSERT_TRUE(statusContains("\"reason\":\"user_bytes_mismatch\""));
    TEST_ASSERT_FALSE(statusContains("\"profile\":{\"requested\":true,\"beforeAvailable\":true,\"needed\":true,\"sent\":true,\"verified\":true"));
    TEST_ASSERT_FALSE(ble.consumeVerifyPushMatchEdge());
}

void test_reserved_user_bits_must_round_trip_exactly_after_supported_overlay() {
    const std::array<uint8_t, 6> before{{0xFF, 0xFF, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> desired{{0xFE, 0xFF, 0xFF, 0xFF, 0x03, 0x00}};
    configureProfile(desired);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot(41039, before));
    queueAndPreflight();
    at(100);
    at(130);

    uint8_t wrong[6];
    std::memcpy(wrong, ble.lastUserBytes, sizeof(wrong));
    wrong[4] ^= 0x40; // Outside the 4.1039 modeled 0x03 mask.
    observeUserBytes(wrong);
    at(130);

    TEST_ASSERT_TRUE(statusContains("user_bytes_mismatch"));
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
    TEST_ASSERT_FALSE(ble.consumeVerifyPushMatchEdge());
}

void test_stale_user_readback_revision_times_out() {
    configureProfile();
    profiles.loadableProfile.detector.displayPolicy = V1DisplayPolicy::Unchanged;
    profiles.loadableProfile.detector.modePolicy = V1ModePolicy::Unchanged;
    profiles.loadableProfile.detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    at(130);
    at(1630);

    TEST_ASSERT_TRUE(statusContains("user_bytes_timeout"));
    TEST_ASSERT_TRUE(statusContains("\"outcome\":\"timeout\""));
}

void test_user_response_arriving_between_write_and_read_request_is_not_fresh_evidence() {
    configureProfile();
    profiles.loadableProfile.detector.displayPolicy = V1DisplayPolicy::Unchanged;
    profiles.loadableProfile.detector.modePolicy = V1ModePolicy::Unchanged;
    profiles.loadableProfile.detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    observeUserBytes(ble.lastUserBytes); // delayed response before 0x11
    at(130);
    at(1630);
    TEST_ASSERT_TRUE(statusContains("user_bytes_timeout"));
    TEST_ASSERT_FALSE(ble.consumeVerifyPushMatchEdge());
}

void test_display_request_echo_and_corrupt_display_cannot_verify() {
    configureProfile();
    profiles.loadableProfile.detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    profiles.loadableProfile.detector.modePolicy = V1ModePolicy::Unchanged;
    profiles.loadableProfile.detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100); // display write

    const auto echo = makeV1Packet(PACKET_ID_TURN_OFF_DISPLAY, {});
    TEST_ASSERT_TRUE(parser.parse(echo.data(), echo.size(), mockMillis));
    observeDisplay(false, 1, 0xEA, true);
    at(1600);

    TEST_ASSERT_TRUE(statusContains("display_timeout"));
    TEST_ASSERT_EQUAL_UINT32(1, parser.displayOnObservationRevision()); // only the priming packet
}

void test_wrong_destination_display_cannot_verify() {
    configureProfile();
    profiles.loadableProfile.detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    profiles.loadableProfile.detector.modePolicy = V1ModePolicy::Unchanged;
    profiles.loadableProfile.detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    observeDisplay(false, 1, 0xEA, false, 0xD6);
    at(1600);

    TEST_ASSERT_TRUE(statusContains("display_timeout"));
    TEST_ASSERT_EQUAL_UINT32(1, parser.displayOnObservationRevision());
}

void test_tolerant_display_interleaving_cannot_replace_canonical_evidence_value() {
    configureProfile();
    profiles.loadableProfile.detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    profiles.loadableProfile.detector.modePolicy = V1ModePolicy::Unchanged;
    profiles.loadableProfile.detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);

    observeDisplay(true, 1);                 // fresh canonical mismatch
    observeDisplay(false, 1, 0xEA, true);    // tolerated render value only
    TEST_ASSERT_FALSE(parser.getDisplayState().displayOn);
    TEST_ASSERT_TRUE(parser.displayOnObservation().value);
    at(1600);

    TEST_ASSERT_TRUE(statusContains("display_mismatch"));
    TEST_ASSERT_FALSE(ble.consumeVerifyPushMatchEdge());
}

void test_fresh_display_mismatches_remain_pending_then_report_mismatch_at_deadline() {
    configureProfile();
    profiles.loadableProfile.detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    profiles.loadableProfile.detector.modePolicy = V1ModePolicy::Unchanged;
    profiles.loadableProfile.detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    observeDisplay(true, 1);
    at(100);
    TEST_ASSERT_TRUE(module.isActive());
    at(1600);
    TEST_ASSERT_TRUE(statusContains("display_mismatch"));
}

void test_mode_requires_fresh_canonical_display_evidence_and_reports_mismatch() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100); // mode write

    const uint32_t primedRevision = parser.modeObservationRevision();
    const auto requestEcho = makeV1Packet(0x36, {2});
    TEST_ASSERT_TRUE(parser.parse(requestEcho.data(), requestEcho.size(), mockMillis));
    observeDisplay(true, 2, 0xEA, true);
    observeDisplay(true, 2, 0xEA, false, 0xD6);
    TEST_ASSERT_EQUAL_UINT32(primedRevision, parser.modeObservationRevision());
    at(1600);
    TEST_ASSERT_TRUE(statusContains("mode_timeout"));

    setUp();
    configureProfile();
    auto& mismatchDetector = profiles.loadableProfile.detector;
    mismatchDetector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    mismatchDetector.displayPolicy = V1DisplayPolicy::Unchanged;
    mismatchDetector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    observeDisplay(true, 1); // canonical D8 infDisplayData, wrong mode
    at(1600);
    TEST_ASSERT_TRUE(statusContains("mode_mismatch"));
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
}

void test_volume_uses_canonical_current_volume_response_for_verification() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100); // write
    at(130); // request 0x37
    observeCurrentVolume(7, 3);
    at(130);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_EQUAL_INT(1, ble.requestCurrentVolumeCalls);
}

void test_volume_readback_mismatch_is_not_success() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    at(130);
    observeCurrentVolume(6, 3);
    at(130);
    TEST_ASSERT_TRUE(statusContains("volume_mismatch"));
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
}

void test_delayed_all_volume_response_cannot_verify_focused_current_volume_read() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    const uint32_t focusedRevision = parser.currentVolumeObservationRevision();
    queueAndPreflight();
    at(100);
    at(130);
    observeAllVolume(7, 3, 5, 2);
    TEST_ASSERT_EQUAL_UINT32(focusedRevision, parser.currentVolumeObservationRevision());
    at(1630);
    TEST_ASSERT_TRUE(statusContains("volume_timeout"));
}

void test_current_volume_response_arriving_before_new_read_request_is_not_fresh_evidence() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    observeCurrentVolume(7, 3); // delayed 0x38 before this operation's 0x37
    at(130);
    at(1630);
    TEST_ASSERT_TRUE(statusContains("volume_timeout"));
}

void test_missing_display_before_state_fails_whole_plan_without_writes() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    auto snapshot = makeSnapshot();
    snapshot.hasDisplayOn = false;
    stageSnapshot(snapshot, false);
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("missing_live_snapshot"));
    TEST_ASSERT_EQUAL_INT(0, ble.setDisplayOnCalls);
}

void test_missing_mode_and_volume_before_observations_each_fail_without_writes() {
    configureProfile();
    auto& modeDetector = profiles.loadableProfile.detector;
    modeDetector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    modeDetector.displayPolicy = V1DisplayPolicy::Unchanged;
    modeDetector.volumePolicy = V1VolumePolicy::Unchanged;
    auto missingMode = makeSnapshot();
    missingMode.hasMode = false;
    stageSnapshot(missingMode, false);
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("missing_live_snapshot"));
    TEST_ASSERT_EQUAL_INT(0, ble.setModeCalls);

    setUp();
    configureProfile();
    auto& volumeDetector = profiles.loadableProfile.detector;
    volumeDetector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    volumeDetector.displayPolicy = V1DisplayPolicy::Unchanged;
    volumeDetector.modePolicy = V1ModePolicy::Unchanged;
    auto missingVolume = makeSnapshot();
    missingVolume.hasCurrentVolume = false;
    stageSnapshot(missingVolume, false);
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("missing_live_snapshot"));
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
}

void test_snapshot_value_must_still_match_latest_canonical_observation_at_preflight() {
    configureProfile();
    profiles.loadableProfile.detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    profiles.loadableProfile.detector.modePolicy = V1ModePolicy::Unchanged;
    profiles.loadableProfile.detector.volumePolicy = V1VolumePolicy::Unchanged;
    const auto snapshot = makeSnapshot();
    stageSnapshot(snapshot);
    observeDisplay(false, 1);
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("missing_live_snapshot"));
    TEST_ASSERT_EQUAL_INT(0, ble.setDisplayOnCalls);
}

void test_missing_user_before_fails_closed_to_preserve_region_and_unknown_bits() {
    configureProfile();
    auto snapshot = makeSnapshot();
    snapshot.hasUserBytes = false;
    stageSnapshot(snapshot);
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("user_bytes_before_required"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setDisplayOnCalls);
}

void test_unsupported_saved_and_pre_41037_temporary_volume_send_zero_writes() {
    configureProfile();
    profiles.loadableProfile.detector.volumePolicy = V1VolumePolicy::Saved;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("unsupported_saved_volume"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);

    setUp();
    configureProfile();
    stageSnapshot(makeSnapshot(41036));
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("unsupported_firmware"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
}

void test_unsupported_old_firmware_preflight_sends_nothing() {
    configureProfile();
    stageSnapshot(makeSnapshot(41017));
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("unsupported_firmware"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setDisplayOnCalls);
}

void test_unverified_future_major_firmware_blocks_every_write() {
    configureProfile();
    stageSnapshot(makeSnapshot(50000));
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("unsupported_firmware"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setDisplayOnCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setModeCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
}

void test_unsupported_bluetooth_led_policy_blocks_whole_plan_without_writes() {
    configureProfile();
    profiles.loadableProfile.detector.bluetoothLedPolicy = static_cast<V1BluetoothLedPolicy>(1);
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("unsupported_bluetooth_led"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setDisplayOnCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setModeCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.requestUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.requestCurrentVolumeCalls);
}

void test_unsupported_custom_frequency_policy_blocks_whole_plan_without_writes() {
    configureProfile();
    profiles.loadableProfile.detector.customFrequencyPolicy = static_cast<V1CustomFrequencyPolicy>(1);
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("unsupported_custom_frequencies"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setDisplayOnCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setModeCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.requestUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.requestCurrentVolumeCalls);
}

void test_vendor_invalid_zero_multibit_user_values_fail_preflight_at_version_boundaries() {
    struct Case {
        uint32_t version;
        std::array<uint8_t, 6> desired;
    };
    const Case cases[] = {
        {41032, {{0xFE, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF}}}, // Ka sensitivity
        {41036, {{0xFE, 0xFF, 0xE7, 0xFF, 0xFF, 0xFF}}}, // Auto Mute
        {41037, {{0xFE, 0xFF, 0x9F, 0xFF, 0xFF, 0xFF}}}, // K sensitivity
        {41037, {{0xFE, 0xFF, 0xFF, 0xFC, 0xFF, 0xFF}}}, // X sensitivity
    };
    for (const auto& test : cases) {
        setUp();
        configureProfile(test.desired);
        auto& detector = profiles.loadableProfile.detector;
        detector.displayPolicy = V1DisplayPolicy::Unchanged;
        detector.modePolicy = V1ModePolicy::Unchanged;
        detector.volumePolicy = V1VolumePolicy::Unchanged;
        stageSnapshot(makeSnapshot(test.version));
        queueAndPreflight();
        TEST_ASSERT_TRUE(statusContains("invalid_user_setting_value"));
        TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    }
}

void test_euro_bit_change_requires_phase_four_custom_frequency_preservation() {
    const std::array<uint8_t, 6> before{{0xFF, 0xFF, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> desired{{0xFE, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureProfile(desired);
    stageSnapshot(makeSnapshot(41039, before));
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("custom_frequency_preservation_required"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
}

void test_advanced_logic_in_existing_euro_mode_is_rejected_before_writes() {
    const std::array<uint8_t, 6> euro{{0xFF, 0xFE, 0xFF, 0xFF, 0xA4, 0x5A}};
    configureProfile(euro);
    profiles.loadableProfile.detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    profiles.loadableProfile.detector.mode = 3;
    stageSnapshot(makeSnapshot(41039, euro));
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("euro_advanced_mode_invalid"));
    TEST_ASSERT_EQUAL_INT(0, ble.setModeCalls);
}

void test_speed_volume_owner_blocks_whole_plan_before_any_detector_write() {
    configureProfile();
    stageSnapshot(makeSnapshot());
    SpeedMuteModule speedMute;
    VolumeFadeModule fade;
    speedMute.begin(true, 25, 3, 0);
    speedMute.state_.muteActive = true;
    TEST_ASSERT_TRUE(quiet.processSpeedVolume(10, speedMute, &fade));
    ble.setVolumeCalls = 0; // discard setup write; preflight itself must send zero
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("volume_owner_busy"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setDisplayOnCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setModeCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
}

void test_active_volume_fade_owner_blocks_whole_plan_before_writes() {
    configureProfile();
    stageSnapshot(makeSnapshot());
    TEST_ASSERT_TRUE(quiet.sendVolume(QuietOwner::VolumeFade, 4, 2));
    ble.setVolumeCalls = 0;
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("volume_owner_busy"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setDisplayOnCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setModeCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
}

void test_successful_fade_restore_releases_apply_while_failed_restore_stays_blocked() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    TEST_ASSERT_TRUE(quiet.sendVolume(QuietOwner::VolumeFade, 4, 2));

    VolumeFadeModule fade;
    fade.activeVolumeOverride = true;
    fade.nextAction.type = VolumeFadeAction::Type::RESTORE;
    fade.nextAction.restoreVolume = 5;
    fade.nextAction.restoreMuteVolume = 2;
    ble.nextVolumeSendResult = SendResult::FAILED;
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(10, &fade));
    TEST_ASSERT_FALSE(quiet.canApplyAutoPushVolumeExactly());

    TEST_ASSERT_TRUE(quiet.executeVolumeFade(20, &fade));
    TEST_ASSERT_FALSE(quiet.canApplyAutoPushVolumeExactly());
    fade.activeVolumeOverride = false; // matching detector echo cleared fade's restore state
    fade.nextAction = VolumeFadeAction{};
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(30, &fade));
    TEST_ASSERT_TRUE(quiet.canApplyAutoPushVolumeExactly());
    ble.setVolumeCalls = 0;
    queueAndPreflight();
    at(100);
    TEST_ASSERT_EQUAL_INT(1, ble.setVolumeCalls);
}

void test_volume_owner_race_is_rechecked_immediately_before_volume_write() {
    configureProfile();
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    verifyUserBytes();
    at(160);
    observeDisplay(false, 1);
    at(160);
    at(190);
    observeDisplay(false, 2);
    at(190);

    SpeedMuteModule speedMute;
    VolumeFadeModule fade;
    speedMute.begin(true, 25, 3, 0);
    speedMute.state_.muteActive = true;
    TEST_ASSERT_TRUE(quiet.processSpeedVolume(200, speedMute, &fade));
    const int competingWriteCalls = ble.setVolumeCalls;
    at(220);

    TEST_ASSERT_TRUE(statusContains("volume_owner_busy"));
    TEST_ASSERT_TRUE(statusContains("\"result\":\"partial\""));
    TEST_ASSERT_EQUAL_INT(competingWriteCalls, ble.setVolumeCalls);
}

void test_volume_verification_lease_defers_competing_owner_until_transaction_finishes() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    VolumeFadeModule fade;
    fade.nextAction.type = VolumeFadeAction::Type::FADE_DOWN;
    fade.nextAction.targetVolume = 1;
    fade.nextAction.targetMuteVolume = 1;
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(110, &fade));
    TEST_ASSERT_EQUAL_INT(0, fade.processCalls);
    TEST_ASSERT_EQUAL_INT(SendResult::NOT_YET,
                          quiet.sendVolumeResult(QuietOwner::VolumeFade, 1, 1));
    TEST_ASSERT_EQUAL_INT(1, ble.setVolumeCalls);
    at(130);
    observeCurrentVolume(7, 3);
    at(130);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_EQUAL_INT(SendResult::SENT,
                          quiet.sendVolumeResult(QuietOwner::VolumeFade, 1, 1));
    TEST_ASSERT_EQUAL_INT(2, ble.setVolumeCalls);
}

void test_session_change_cancels_operation_and_blocks_remaining_components() {
    configureProfile();
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    ble.setSessionGeneration(2);
    at(100);
    TEST_ASSERT_TRUE(statusContains("session_changed"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
}

void test_new_session_snapshot_terminates_old_operation_without_erasing_new_capture() {
    configureProfile();
    stageSnapshot(makeSnapshot());
    TEST_ASSERT_EQUAL_INT(AutoPushModule::QueueResult::QUEUED, module.queueSlotPush(0));

    ble.setSessionGeneration(2);
    parser.resetV1Version();
    parser.resetModeAndDisplayState();
    parser.resetVolumeState();
    ble.resetSessionSettingsCapture();
    auto replacement = makeSnapshot();
    replacement.sessionGeneration = 2;
    stageSnapshot(replacement);

    TEST_ASSERT_EQUAL_INT(AutoPushModule::QueueResult::QUEUED, module.queueSlotPush(0));
    TEST_ASSERT_TRUE(module.isActive());
    at(100); // replacement WaitReady -> LoadProfile
    at(100); // LoadProfile -> Preflight
    at(100); // session-2 snapshot admitted
    at(100); // first needed user-byte write
    TEST_ASSERT_EQUAL_INT(1, ble.writeUserBytesCalls);
    TEST_ASSERT_FALSE(statusContains("missing_live_snapshot"));
    TEST_ASSERT_FALSE(statusContains("session_changed"));
}

void test_disconnect_after_user_verification_reports_partial_and_blocks_later_writes() {
    configureProfile();
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    verifyUserBytes();
    ble.setConnected(false);
    at(160);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"partial\""));
    TEST_ASSERT_TRUE(statusContains("\"reason\":\"disconnected\""));
    TEST_ASSERT_EQUAL_INT(0, ble.setDisplayOnCalls);
}

void test_stale_snapshot_and_one_shot_reuse_fail_before_writes() {
    configureProfile();
    auto snapshot = makeSnapshot();
    snapshot.capturedUptimeMs = 1;
    stageSnapshot(snapshot);
    mockMillis = 6002;
    TEST_ASSERT_EQUAL_INT(AutoPushModule::QueueResult::QUEUED, module.queueSlotPush(0));
    at(6102);
    at(6102);
    at(6102);
    TEST_ASSERT_TRUE(statusContains("missing_live_snapshot"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);

    mockMillis = 6200;
    TEST_ASSERT_EQUAL_INT(AutoPushModule::QueueResult::QUEUED, module.queueSlotPush(0));
    at(6300);
    at(6300);
    at(6300);
    TEST_ASSERT_TRUE(statusContains("missing_live_snapshot"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
}

void test_queued_before_request_user_evidence_cannot_verify_after_late_processing() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100); // write
    const uint32_t queuedBeforeRequest = ble.noteV1NotificationIngress();
    at(130); // focused read request; boundary includes the queued callback
    observeUserBytes(ble.lastUserBytes, queuedBeforeRequest); // parsed late
    at(1630);

    TEST_ASSERT_TRUE(statusContains("user_bytes_timeout"));
    TEST_ASSERT_FALSE(ble.consumeVerifyPushMatchEdge());
}

void test_queued_before_command_display_and_mode_evidence_cannot_verify() {
    configureProfile();
    auto& displayOnly = profiles.loadableProfile.detector;
    displayOnly.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    displayOnly.modePolicy = V1ModePolicy::Unchanged;
    displayOnly.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    const uint32_t queuedBeforeDisplayCommand = ble.noteV1NotificationIngress();
    at(100);
    observeDisplay(false, 1, 0xEA, false, 0xD8, queuedBeforeDisplayCommand);
    // A direct parser call without BLE ingress provenance is also ineligible.
    observeDisplay(false, 1, 0xEA, false, 0xD8, 0);
    at(1600);
    TEST_ASSERT_TRUE(statusContains("display_timeout"));

    setUp();
    configureProfile();
    auto& modeOnly = profiles.loadableProfile.detector;
    modeOnly.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    modeOnly.displayPolicy = V1DisplayPolicy::Unchanged;
    modeOnly.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    const uint32_t queuedBeforeModeCommand = ble.noteV1NotificationIngress();
    at(100);
    observeDisplay(true, 2, 0xEA, false, 0xD8, queuedBeforeModeCommand);
    at(1600);
    TEST_ASSERT_TRUE(statusContains("mode_timeout"));
}

void test_queued_before_request_volume_evidence_cannot_verify_after_late_processing() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100); // write
    const uint32_t queuedBeforeRequest = ble.noteV1NotificationIngress();
    at(130); // 0x37 request
    observeCurrentVolume(7, 3, 0xEA, false, 0xD6, queuedBeforeRequest);
    at(1630);

    TEST_ASSERT_TRUE(statusContains("volume_timeout"));
    TEST_ASSERT_FALSE(ble.consumeVerifyPushMatchEdge());
}

void test_ingress_sequence_wrap_skips_zero_and_accepts_only_the_later_response() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    ble.v1NotificationIngressSequenceValue = UINT32_MAX;
    at(130);
    observeUserBytes(ble.lastUserBytes); // wraps to 1, never zero
    at(130);

    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_TRUE(ble.consumeVerifyPushMatchEdge());
}

void test_terminal_session_recheck_prevents_success_and_generation_qualifies_edge() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    at(130);
    observeUserBytes(ble.lastUserBytes);
    ble.changeSessionOnNextUserBytesCopy = true;
    at(130);
    TEST_ASSERT_TRUE(statusContains("session_changed"));
    TEST_ASSERT_FALSE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_FALSE(ble.consumeVerifyPushMatchEdge());

    setUp();
    configureProfile();
    auto& edgeDetector = profiles.loadableProfile.detector;
    edgeDetector.displayPolicy = V1DisplayPolicy::Unchanged;
    edgeDetector.modePolicy = V1ModePolicy::Unchanged;
    edgeDetector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    at(130);
    observeUserBytes(ble.lastUserBytes);
    ble.changeSessionDuringPublish = true;
    at(130);
    TEST_ASSERT_FALSE(ble.consumeVerifyPushMatchEdge());
}

void test_responses_ingressed_before_send_returns_cannot_verify_the_operation() {
    configureProfile();
    auto& userOnly = profiles.loadableProfile.detector;
    userOnly.displayPolicy = V1DisplayPolicy::Unchanged;
    userOnly.modePolicy = V1ModePolicy::Unchanged;
    userOnly.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    ble.requestUserBytesSendHook = injectMatchingUserBytesDuringSend;
    at(130);
    at(1630);
    TEST_ASSERT_TRUE(statusContains("user_bytes_timeout"));

    setUp();
    configureProfile();
    auto& displayOnly = profiles.loadableProfile.detector;
    displayOnly.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    displayOnly.modePolicy = V1ModePolicy::Unchanged;
    displayOnly.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    ble.setDisplayOnSendHook = injectMatchingDisplayDuringSend;
    at(100);
    at(1600);
    TEST_ASSERT_TRUE(statusContains("display_timeout"));

    setUp();
    configureProfile();
    auto& modeOnly = profiles.loadableProfile.detector;
    modeOnly.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    modeOnly.displayPolicy = V1DisplayPolicy::Unchanged;
    modeOnly.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    ble.setModeSendHook = injectMatchingModeDuringSend;
    at(100);
    at(1600);
    TEST_ASSERT_TRUE(statusContains("mode_timeout"));

    setUp();
    configureProfile();
    auto& volumeOnly = profiles.loadableProfile.detector;
    volumeOnly.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    volumeOnly.displayPolicy = V1DisplayPolicy::Unchanged;
    volumeOnly.modePolicy = V1ModePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    ble.requestCurrentVolumeSendHook = injectMatchingVolumeDuringSend;
    at(130);
    at(1630);
    TEST_ASSERT_TRUE(statusContains("volume_timeout"));
}

void test_write_and_read_failures_are_distinct_and_never_applied() {
    configureProfile();
    profiles.loadableProfile.detector.displayPolicy = V1DisplayPolicy::Unchanged;
    profiles.loadableProfile.detector.modePolicy = V1ModePolicy::Unchanged;
    profiles.loadableProfile.detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    ble.writeUserBytesResult = false;
    queueAndPreflight();
    at(100);
    TEST_ASSERT_TRUE(statusContains("user_bytes_write_failed"));

    setUp();
    configureProfile();
    profiles.loadableProfile.detector.displayPolicy = V1DisplayPolicy::Unchanged;
    profiles.loadableProfile.detector.modePolicy = V1ModePolicy::Unchanged;
    profiles.loadableProfile.detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    ble.requestUserBytesResult = false;
    queueAndPreflight();
    at(100);
    ble.startUserBytesVerification(ble.lastUserBytes);
    at(130);
    TEST_ASSERT_TRUE(statusContains("user_bytes_read_failed"));
    observeUserBytes(ble.lastUserBytes);
    TEST_ASSERT_FALSE(ble.consumeVerifyPushMatchEdge());
}

void test_display_mode_and_volume_transport_failures_are_distinct_and_release_volume_lease() {
    configureProfile();
    auto& displayDetector = profiles.loadableProfile.detector;
    displayDetector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    displayDetector.modePolicy = V1ModePolicy::Unchanged;
    displayDetector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    ble.setDisplayOnResult = false;
    queueAndPreflight();
    at(100);
    TEST_ASSERT_TRUE(statusContains("display_write_failed"));
    TEST_ASSERT_EQUAL_INT(0, ble.setModeCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);

    setUp();
    configureProfile();
    auto& modeDetector = profiles.loadableProfile.detector;
    modeDetector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    modeDetector.displayPolicy = V1DisplayPolicy::Unchanged;
    modeDetector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    ble.setModeResult = false;
    queueAndPreflight();
    at(100);
    TEST_ASSERT_TRUE(statusContains("mode_write_failed"));
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);

    setUp();
    configureProfile();
    auto& volumeWriteDetector = profiles.loadableProfile.detector;
    volumeWriteDetector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    volumeWriteDetector.displayPolicy = V1DisplayPolicy::Unchanged;
    volumeWriteDetector.modePolicy = V1ModePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    ble.setVolumeSuccess = false;
    queueAndPreflight();
    at(100);
    TEST_ASSERT_TRUE(statusContains("volume_write_failed"));
    ble.setVolumeSuccess = true;
    TEST_ASSERT_EQUAL_INT(SendResult::SENT,
                          quiet.sendVolumeResult(QuietOwner::VolumeFade, 1, 1));

    setUp();
    configureProfile();
    auto& volumeReadDetector = profiles.loadableProfile.detector;
    volumeReadDetector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    volumeReadDetector.displayPolicy = V1DisplayPolicy::Unchanged;
    volumeReadDetector.modePolicy = V1ModePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    ble.requestCurrentVolumeResult = false;
    queueAndPreflight();
    at(100);
    at(130);
    TEST_ASSERT_TRUE(statusContains("volume_read_failed"));
    TEST_ASSERT_EQUAL_INT(SendResult::SENT,
                          quiet.sendVolumeResult(QuietOwner::VolumeFade, 1, 1));
    TEST_ASSERT_FALSE(ble.consumeVerifyPushMatchEdge());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_full_apply_requires_fresh_canonical_readbacks_for_every_component);
    RUN_TEST(test_all_noop_components_are_proven_unchanged_without_writes);
    RUN_TEST(test_supported_user_masks_match_every_vendor_boundary);
    RUN_TEST(test_supported_masks_preserve_unknown_bits_and_four_byte_firmware_shape);
    RUN_TEST(test_41039_masks_only_modeled_byte_four_bits_and_preserves_byte_five);
    RUN_TEST(test_alp_laser_override_is_in_effective_bytes_and_verified);
    RUN_TEST(test_user_readback_mismatch_is_failure_not_applied);
    RUN_TEST(test_reserved_user_bits_must_round_trip_exactly_after_supported_overlay);
    RUN_TEST(test_stale_user_readback_revision_times_out);
    RUN_TEST(test_user_response_arriving_between_write_and_read_request_is_not_fresh_evidence);
    RUN_TEST(test_display_request_echo_and_corrupt_display_cannot_verify);
    RUN_TEST(test_wrong_destination_display_cannot_verify);
    RUN_TEST(test_tolerant_display_interleaving_cannot_replace_canonical_evidence_value);
    RUN_TEST(test_fresh_display_mismatches_remain_pending_then_report_mismatch_at_deadline);
    RUN_TEST(test_mode_requires_fresh_canonical_display_evidence_and_reports_mismatch);
    RUN_TEST(test_volume_uses_canonical_current_volume_response_for_verification);
    RUN_TEST(test_volume_readback_mismatch_is_not_success);
    RUN_TEST(test_delayed_all_volume_response_cannot_verify_focused_current_volume_read);
    RUN_TEST(test_current_volume_response_arriving_before_new_read_request_is_not_fresh_evidence);
    RUN_TEST(test_missing_display_before_state_fails_whole_plan_without_writes);
    RUN_TEST(test_missing_mode_and_volume_before_observations_each_fail_without_writes);
    RUN_TEST(test_snapshot_value_must_still_match_latest_canonical_observation_at_preflight);
    RUN_TEST(test_missing_user_before_fails_closed_to_preserve_region_and_unknown_bits);
    RUN_TEST(test_unsupported_saved_and_pre_41037_temporary_volume_send_zero_writes);
    RUN_TEST(test_unsupported_old_firmware_preflight_sends_nothing);
    RUN_TEST(test_unverified_future_major_firmware_blocks_every_write);
    RUN_TEST(test_unsupported_bluetooth_led_policy_blocks_whole_plan_without_writes);
    RUN_TEST(test_unsupported_custom_frequency_policy_blocks_whole_plan_without_writes);
    RUN_TEST(test_vendor_invalid_zero_multibit_user_values_fail_preflight_at_version_boundaries);
    RUN_TEST(test_euro_bit_change_requires_phase_four_custom_frequency_preservation);
    RUN_TEST(test_advanced_logic_in_existing_euro_mode_is_rejected_before_writes);
    RUN_TEST(test_speed_volume_owner_blocks_whole_plan_before_any_detector_write);
    RUN_TEST(test_active_volume_fade_owner_blocks_whole_plan_before_writes);
    RUN_TEST(test_successful_fade_restore_releases_apply_while_failed_restore_stays_blocked);
    RUN_TEST(test_volume_owner_race_is_rechecked_immediately_before_volume_write);
    RUN_TEST(test_volume_verification_lease_defers_competing_owner_until_transaction_finishes);
    RUN_TEST(test_session_change_cancels_operation_and_blocks_remaining_components);
    RUN_TEST(test_new_session_snapshot_terminates_old_operation_without_erasing_new_capture);
    RUN_TEST(test_disconnect_after_user_verification_reports_partial_and_blocks_later_writes);
    RUN_TEST(test_stale_snapshot_and_one_shot_reuse_fail_before_writes);
    RUN_TEST(test_queued_before_request_user_evidence_cannot_verify_after_late_processing);
    RUN_TEST(test_queued_before_command_display_and_mode_evidence_cannot_verify);
    RUN_TEST(test_queued_before_request_volume_evidence_cannot_verify_after_late_processing);
    RUN_TEST(test_ingress_sequence_wrap_skips_zero_and_accepts_only_the_later_response);
    RUN_TEST(test_terminal_session_recheck_prevents_success_and_generation_qualifies_edge);
    RUN_TEST(test_responses_ingressed_before_send_returns_cannot_verify_the_operation);
    RUN_TEST(test_write_and_read_failures_are_distinct_and_never_applied);
    RUN_TEST(test_display_mode_and_volume_transport_failures_are_distinct_and_release_volume_lease);
    return UNITY_END();
}
