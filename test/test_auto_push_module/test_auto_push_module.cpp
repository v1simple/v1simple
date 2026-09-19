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
#define PACKET_ID_FACTORY_DEFAULT 0x14
#define PACKET_ID_WRITE_SWEEP_DEFINITION 0x15
#define PACKET_ID_REQ_ALL_SWEEP_DEFINITIONS 0x16
#define PACKET_ID_RESP_SWEEP_DEFINITION 0x17
#define PACKET_ID_REQ_MAX_SWEEP_INDEX 0x19
#define PACKET_ID_RESP_MAX_SWEEP_INDEX 0x20
#define PACKET_ID_RESP_SWEEP_WRITE_RESULT 0x21
#define PACKET_ID_REQ_SWEEP_SECTIONS 0x22
#define PACKET_ID_RESP_SWEEP_SECTIONS 0x23
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
                    uint8_t destination = 0xD8, uint32_t ingressSequence = UINT32_MAX,
                    V1BluetoothIndicatorState bluetooth = V1BluetoothIndicatorState::Off) {
    const uint8_t aux0 = static_cast<uint8_t>(0x04 | (on ? 0x08 : 0x00));
    uint8_t aux1 = static_cast<uint8_t>((mode & 0x03) << 2);
    if (bluetooth == V1BluetoothIndicatorState::Blinking) aux1 |= 0x40;
    else if (bluetooth == V1BluetoothIndicatorState::On) aux1 |= 0xC0;
    auto packet = makeV1Packet(PACKET_ID_DISPLAY_DATA,
                               {0x3F, 0x3F, 0x00, 0x00, 0x00, aux0,
                                aux1, 0x52}, origin, destination);
    if (corruptChecksum && origin == 0xEA) packet[packet.size() - 2] ^= 0x01;
    if (ingressSequence == UINT32_MAX) ingressSequence = ble.noteV1NotificationIngress();
    const bool parsed = parser.parse(packet.data(), packet.size(), mockMillis, ingressSequence);
    const bool expected = !corruptChecksum && destination == 0xD8 &&
                          (origin == 0xEA || origin == 0xE9);
    TEST_ASSERT_EQUAL(expected, parsed);
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

void observeAllVolume(uint8_t main, uint8_t muted, uint8_t savedMain, uint8_t savedMuted,
                      uint8_t origin = 0xEA, bool corruptChecksum = false,
                      uint8_t destination = 0xD6, uint32_t ingressSequence = UINT32_MAX) {
    auto packet = makeV1Packet(PACKET_ID_RESP_ALL_VOLUME, {main, muted, savedMain, savedMuted},
                               origin, destination);
    if (corruptChecksum && origin == 0xEA) packet[packet.size() - 2] ^= 0x01;
    if (ingressSequence == UINT32_MAX) ingressSequence = ble.noteV1NotificationIngress();
    const bool parsed = parser.parse(packet.data(), packet.size(), mockMillis, ingressSequence);
    TEST_ASSERT_EQUAL(!corruptChecksum && destination == 0xD6, parsed);
}

void observeSweepMax(uint8_t maxIndex, uint32_t ingressSequence = UINT32_MAX,
                     bool corruptChecksum = false, uint8_t destination = 0xD6) {
    auto packet = makeV1Packet(PACKET_ID_RESP_MAX_SWEEP_INDEX, {maxIndex}, 0xEA, destination);
    if (corruptChecksum) packet[packet.size() - 2] ^= 0x01;
    if (ingressSequence == UINT32_MAX) ingressSequence = ble.noteV1NotificationIngress();
    const bool parsed = parser.parse(packet.data(), packet.size(), mockMillis, ingressSequence);
    TEST_ASSERT_EQUAL(!corruptChecksum && destination == 0xD6, parsed);
}

void observeSweepSections(const V1DetectorSnapshot& snapshot,
                          uint32_t ingressSequence = UINT32_MAX) {
    const auto hi = [](uint16_t value) { return static_cast<uint8_t>(value >> 8); };
    const auto lo = [](uint16_t value) { return static_cast<uint8_t>(value & 0xFF); };
    TEST_ASSERT_GREATER_THAN_UINT8(0, snapshot.sweepSectionCount);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(3, snapshot.sweepSectionCount);
    std::vector<uint8_t> data;
    data.reserve(static_cast<size_t>(snapshot.sweepSectionCount) * 5u);
    for (uint8_t index = 0; index < snapshot.sweepSectionCount; ++index) {
        const auto& section = snapshot.sweepSections[index];
        data.push_back(static_cast<uint8_t>(((index + 1u) << 4u) | snapshot.sweepSectionCount));
        data.push_back(hi(section.upperMHz));
        data.push_back(lo(section.upperMHz));
        data.push_back(hi(section.lowerMHz));
        data.push_back(lo(section.lowerMHz));
    }
    const auto packet = makeV1Packet(PACKET_ID_RESP_SWEEP_SECTIONS, data);
    if (ingressSequence == UINT32_MAX) ingressSequence = ble.noteV1NotificationIngress();
    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), mockMillis, ingressSequence));
}

void observeSweepDefinition(uint8_t index, uint16_t lower, uint16_t upper,
                            uint32_t ingressSequence = UINT32_MAX,
                            bool corruptChecksum = false, uint8_t destination = 0xD6,
                            bool expectCollectorConflict = false) {
    auto packet = makeV1Packet(PACKET_ID_RESP_SWEEP_DEFINITION,
                               {static_cast<uint8_t>(0x80u | index),
                                static_cast<uint8_t>(upper >> 8), static_cast<uint8_t>(upper & 0xFF),
                                static_cast<uint8_t>(lower >> 8), static_cast<uint8_t>(lower & 0xFF)},
                               0xEA, destination);
    if (corruptChecksum) packet[packet.size() - 2] ^= 0x01;
    if (ingressSequence == UINT32_MAX) ingressSequence = ble.noteV1NotificationIngress();
    const bool parsed = parser.parse(packet.data(), packet.size(), mockMillis, ingressSequence);
    TEST_ASSERT_EQUAL(!corruptChecksum && destination == 0xD6 && !expectCollectorConflict, parsed);
}

void observeSweepWriteResult(uint8_t result, uint32_t ingressSequence = UINT32_MAX) {
    const auto packet = makeV1Packet(PACKET_ID_RESP_SWEEP_WRITE_RESULT, {result});
    if (ingressSequence == UINT32_MAX) ingressSequence = ble.noteV1NotificationIngress();
    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), mockMillis, ingressSequence));
}

void observeCapturedSweepSections(const V1DetectorSnapshot& snapshot) {
    const uint32_t ingress = ble.noteV1NotificationIngress();
    TEST_ASSERT_TRUE(ble.sessionSweepResponseEligible(
        PACKET_ID_RESP_SWEEP_SECTIONS, ingress));
    if (ble.consumeSessionSweepParserReset(PACKET_ID_RESP_SWEEP_SECTIONS)) {
        parser.resetSweepSectionsObservation();
    }
    observeSweepSections(snapshot, ingress);
    const auto& observed = parser.sweepSectionsObservation();
    ble.onSweepSectionsReceived(observed.available && observed.complete &&
                                !observed.poisoned);
}

void observeCapturedSweepMax(uint8_t maxIndex) {
    const uint32_t ingress = ble.noteV1NotificationIngress();
    TEST_ASSERT_TRUE(ble.sessionSweepResponseEligible(
        PACKET_ID_RESP_MAX_SWEEP_INDEX, ingress));
    if (ble.consumeSessionSweepParserReset(PACKET_ID_RESP_MAX_SWEEP_INDEX)) {
        parser.resetSweepMaxObservation();
    }
    observeSweepMax(maxIndex, ingress);
    const auto& observed = parser.sweepMaxObservation();
    ble.onSweepMaxReceived(observed.available && !observed.poisoned);
}

void observeCapturedSweepDefinition(uint8_t index, uint16_t lower, uint16_t upper) {
    const uint32_t ingress = ble.noteV1NotificationIngress();
    TEST_ASSERT_TRUE(ble.sessionSweepResponseEligible(
        PACKET_ID_RESP_SWEEP_DEFINITION, ingress));
    if (ble.consumeSessionSweepParserReset(PACKET_ID_RESP_SWEEP_DEFINITION)) {
        parser.resetSweepDefinitionsObservation();
    }
    observeSweepDefinition(index, lower, upper, ingress);
    const auto& maximum = parser.sweepMaxObservation();
    const auto& definitions = parser.sweepDefinitionsObservation();
    const uint64_t required = maximum.maxIndex == 63
                                  ? UINT64_MAX
                                  : ((uint64_t{1} << (maximum.maxIndex + 1u)) - 1u);
    ble.onSweepDefinitionsReceived(maximum.available && !maximum.poisoned &&
                                   !definitions.poisoned &&
                                   definitions.presentMask == required);
}

void completeTargetRegionSweepRefresh(const V1DetectorSnapshot& target,
                                      unsigned long verifyAt) {
    observeCapturedSweepSections(target);
    observeCapturedSweepMax(target.maxSweepIndex);
    for (uint8_t index = 0; index <= target.maxSweepIndex; ++index) {
        const auto& definition = target.sweepDefinitions[index];
        observeCapturedSweepDefinition(index, definition.lowerMHz,
                                       definition.upperMHz);
    }
    mockMillis = verifyAt;
    module.process();
}

void observeUserBytes(const uint8_t* bytes, uint32_t ingressSequence = UINT32_MAX) {
    if (ingressSequence == UINT32_MAX) ingressSequence = ble.noteV1NotificationIngress();
    ble.onUserBytesReceived(bytes, ingressSequence);
}

void injectMatchingUserBytesDuringSend() { observeUserBytes(ble.lastUserBytes); }
void injectMatchingDisplayDuringSend() { observeDisplay(ble.lastDisplayOnValue, 1); }
void injectMatchingModeDuringSend() { observeDisplay(true, ble.lastModeValue); }
void injectMatchingVolumeDuringSend() { observeCurrentVolume(ble.lastVolume, ble.lastMuteVolume); }
void injectMatchingAllVolumeDuringSend() {
    observeAllVolume(ble.lastVolume, ble.lastMuteVolume, 4, 1);
}

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
    snapshot.hasBluetoothIndicator = true;
    snapshot.bluetoothIndicator = V1BluetoothIndicatorState::Off;
    snapshot.hasMode = true;
    snapshot.mode = 'A';
    snapshot.hasCurrentVolume = true;
    snapshot.currentMainVolume = 5;
    snapshot.currentMutedVolume = 2;
    snapshot.hasSavedVolume = true;
    snapshot.savedMainVolume = 4;
    snapshot.savedMutedVolume = 1;
    return snapshot;
}

void stageSnapshot(const V1DetectorSnapshot& snapshot, bool primeObservations = true) {
    parseVersion(snapshot.firmwareVersion);
    if (snapshot.hasUserBytes) {
        ble.beginSessionUserBytesCapture(ble.latestV1NotificationIngressSequence());
        observeUserBytes(snapshot.userBytes.data());
    }
    if (primeObservations && snapshot.hasDisplayOn && snapshot.hasMode) {
        observeDisplay(snapshot.displayOn, snapshot.mode == 'L' ? 3 : snapshot.mode == 'l' ? 2 : 1,
                       0xEA, false, 0xD8, UINT32_MAX, snapshot.bluetoothIndicator);
    }
    if (primeObservations && snapshot.hasCurrentVolume && snapshot.hasSavedVolume) {
        ble.beginSessionAllVolumeCapture(ble.latestV1NotificationIngressSequence());
        observeAllVolume(snapshot.currentMainVolume, snapshot.currentMutedVolume,
                         snapshot.savedMainVolume, snapshot.savedMutedVolume);
    }
    if (primeObservations && snapshot.hasCurrentVolume) {
        observeCurrentVolume(snapshot.currentMainVolume, snapshot.currentMutedVolume);
    }
    if (primeObservations && snapshot.hasSweepSections) {
        parser.resetSweepSectionsObservation();
        observeSweepSections(snapshot);
    }
    if (primeObservations && snapshot.hasMaxSweepIndex) {
        parser.resetSweepMaxObservation();
        observeSweepMax(snapshot.maxSweepIndex);
    }
    if (primeObservations && snapshot.hasSweepDefinitions) {
        parser.resetSweepDefinitionsObservation();
        for (size_t index = 0; index <= snapshot.maxSweepIndex; ++index) {
            const auto& definition = snapshot.sweepDefinitions[index];
            observeSweepDefinition(definition.index, definition.lowerMHz, definition.upperMHz);
        }
    }
    module.setPreApplySnapshot(snapshot);
}

void addSweepSnapshot(V1DetectorSnapshot& snapshot) {
    snapshot.hasSweepSections = true;
    snapshot.sweepSectionCount = 2;
    snapshot.sweepSections = {{{0, 2, 23900, 25000}, {1, 2, 33000, 37000}}};
    snapshot.hasMaxSweepIndex = true;
    snapshot.maxSweepIndex = 3;
    snapshot.hasSweepDefinitions = true;
    snapshot.sweepDefinitions = {{{0, 24000, 24100}, {1, 0, 0},
                                  {2, 34000, 34100}, {3, 0, 0}}};
}

void configureCustomOnly(const std::vector<V1CustomFrequencyDefinition>& definitions) {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    TEST_ASSERT_TRUE(detector.customFrequencyDefinitions.assign(definitions));
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
    observeAllVolume(7, 3, 4, 1);
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
    g_autoPushAdmissionFailurePointForTest = AutoPushAdmissionFailurePoint::None;
}

void tearDown() {}

void test_queue_rejects_failed_active_slot_persistence_before_operation_or_detector_write() {
    configureProfile();
    settings.settings.activeSlot = 0;
    settings.slotConfigs[1].profileName = "ROAD";
    stageSnapshot(makeSnapshot());
    settings.setActiveSlotSuccess = false;

    TEST_ASSERT_EQUAL_INT(AutoPushModule::QueueResult::ACTIVE_SLOT_PERSIST_FAILED,
                          module.queueSlotPush(1, true));
    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_EQUAL_UINT8(0, settings.settings.activeSlot);
    TEST_ASSERT_EQUAL_INT(1, settings.setActiveSlotCalls);
    TEST_ASSERT_EQUAL_INT(0, display.drawProfileIndicatorCalls);
    at(1000);
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setDisplayOnCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setModeCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"none\""));
}

void test_queue_stages_every_owned_string_and_definition_capacity_before_activation() {
    const AutoPushAdmissionFailurePoint failures[] = {
        AutoPushAdmissionFailurePoint::OperationStorage,
        AutoPushAdmissionFailurePoint::SlotProfile,
        AutoPushAdmissionFailurePoint::ProfileName,
        AutoPushAdmissionFailurePoint::ProfileDescription,
        AutoPushAdmissionFailurePoint::StatusProfile,
        AutoPushAdmissionFailurePoint::DefinitionCapacity,
    };
    for (const AutoPushAdmissionFailurePoint failure : failures) {
        setUp();
        configureProfile();
        settings.settings.activeSlot = 0;
        settings.slotConfigs[1].profileName = "ROAD";
        profiles.loadableProfile.description = "A description that requires owned storage";
        stageSnapshot(makeSnapshot());
        g_autoPushAdmissionFailurePointForTest = failure;

        TEST_ASSERT_EQUAL_INT(AutoPushModule::QueueResult::STAGING_UNAVAILABLE,
                              module.queueSlotPush(1, true));
        TEST_ASSERT_FALSE(module.isActive());
        TEST_ASSERT_EQUAL_UINT8(0, settings.settings.activeSlot);
        TEST_ASSERT_EQUAL_INT(0, settings.setActiveSlotCalls);
        TEST_ASSERT_EQUAL_INT(0, display.drawProfileIndicatorCalls);
        at(1000);
        TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
        TEST_ASSERT_EQUAL_INT(0, ble.setDisplayOnCalls);
        TEST_ASSERT_EQUAL_INT(0, ble.setModeCalls);
        TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
        TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
    }
}

void test_queue_secures_maximum_v3_profile_before_activation_and_preserves_legacy_load_step() {
    configureProfile();
    String maximumName;
    for (size_t index = 0; index < 64; ++index) maximumName += 'N';
    String maximumDescription;
    for (size_t index = 0; index < 4096; ++index) {
        maximumDescription += 'D';
    }
    settings.settings.activeSlot = 0;
    settings.slotConfigs[1].profileName = maximumName;
    profiles.loadableProfileName = maximumName;
    profiles.loadableProfile.name = maximumName;
    profiles.loadableProfile.description = maximumDescription;
    profiles.loadableProfile.detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    profiles.loadableProfile.detector.customFrequencyDefinitions.clear();
    for (uint8_t index = 0; index < 64; ++index) {
        profiles.loadableProfile.detector.customFrequencyDefinitions.push_back(
            V1CustomFrequencyDefinition{index, 0, 0});
    }
    stageSnapshot(makeSnapshot());

    TEST_ASSERT_EQUAL_INT(AutoPushModule::QueueResult::QUEUED,
                          module.queueSlotPush(1, true));
    TEST_ASSERT_TRUE(module.isActive());
    TEST_ASSERT_EQUAL_UINT8(1, settings.settings.activeSlot);
    TEST_ASSERT_EQUAL_INT(1, settings.setActiveSlotCalls);
    TEST_ASSERT_EQUAL_INT(1, display.drawProfileIndicatorCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);

    setUp();
    configureProfile();
    settings.settings.autoPushProfileSchemaVersion = 0;
    settings.settings.activeSlot = 0;
    settings.slotConfigs[1].profileName = "ROAD";
    profiles.loadableProfile.description = "Legacy load still stages before activation";
    stageSnapshot(makeSnapshot());
    g_autoPushAdmissionFailurePointForTest =
        AutoPushAdmissionFailurePoint::ProfileDescription;
    TEST_ASSERT_EQUAL_INT(AutoPushModule::QueueResult::STAGING_UNAVAILABLE,
                          module.queueSlotPush(1, true));
    TEST_ASSERT_EQUAL_UINT8(0, settings.settings.activeSlot);
    TEST_ASSERT_EQUAL_INT(0, settings.setActiveSlotCalls);
    TEST_ASSERT_FALSE(module.isActive());
}

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
    TEST_ASSERT_EQUAL_INT(1, ble.requestAllVolumeCalls);
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
    detector.volumePolicy = V1VolumePolicy::Saved;
    detector.mainVolume = 5;
    detector.mutedVolume = 2;
    auto snapshot = makeSnapshot(41039, bytes);
    snapshot.savedMainVolume = 5;
    snapshot.savedMutedVolume = 2;
    stageSnapshot(snapshot);
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

void test_rejected_display_interleaving_cannot_replace_canonical_evidence_value() {
    configureProfile();
    profiles.loadableProfile.detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    profiles.loadableProfile.detector.modePolicy = V1ModePolicy::Unchanged;
    profiles.loadableProfile.detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);

    observeDisplay(true, 1);                 // fresh canonical mismatch
    observeDisplay(false, 1, 0xEA, true);    // rejected before render/evidence mutation
    TEST_ASSERT_TRUE(parser.getDisplayState().displayOn);
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

void test_temporary_volume_uses_all_volume_readback_and_preserves_saved_pair() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumeFeedback = V1VolumeFeedbackPolicy::ChangedOnly;
    detector.volumeDisconnect = V1VolumeDisconnectPolicy::KeepCurrent;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100); // write
    TEST_ASSERT_EQUAL_HEX8(0x0B, ble.lastVolumeAux);
    at(130); // request 0x3c
    observeAllVolume(7, 3, 4, 1);
    at(130);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_EQUAL_INT(1, ble.requestAllVolumeCalls);
    JsonDocument status;
    const String json = module.getStatusJson();
    TEST_ASSERT_FALSE(deserializeJson(status, json.c_str()));
    JsonObject volume = status["components"]["volume"];
    TEST_ASSERT_TRUE(volume["verified"].as<bool>());
    TEST_ASSERT_TRUE(volume["valuesVerified"].as<bool>());
    TEST_ASSERT_FALSE(volume["policyReadbackAvailable"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("sent_unobservable", volume["policyOutcome"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("current_values_and_saved_preservation_only",
                             volume["verificationScope"].as<const char*>());
}

void test_saved_volume_sets_save_aux_and_verifies_both_current_and_saved_pairs() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Saved;
    detector.volumeFeedback = V1VolumeFeedbackPolicy::Always;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    TEST_ASSERT_EQUAL_HEX8(0x05, ble.lastVolumeAux);
    at(130);
    observeAllVolume(7, 3, 7, 3);
    at(130);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_TRUE(statusContains("\"verificationScope\":\"current_and_saved_values_only\""));
}

void test_volume_behavior_aux_is_sent_even_when_values_already_match() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.mainVolume = 5;
    detector.mutedVolume = 2;
    detector.volumeFeedback = V1VolumeFeedbackPolicy::None;
    detector.volumeDisconnect = V1VolumeDisconnectPolicy::RestoreSaved;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    TEST_ASSERT_EQUAL_INT(1, ble.setVolumeCalls);
    // A previous keep-current command is not readable. Restore-saved b3=0
    // therefore still requires an explicit command even when values match.
    TEST_ASSERT_EQUAL_HEX8(0x00, ble.lastVolumeAux);
    at(130);
    observeAllVolume(5, 2, 4, 1);
    at(130);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
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
    observeAllVolume(6, 3, 4, 1);
    at(130);
    TEST_ASSERT_TRUE(statusContains("volume_mismatch"));
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
}

void test_wrong_destination_and_corrupt_all_volume_responses_cannot_verify() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    at(130);
    observeAllVolume(7, 3, 4, 1, 0xEA, false, 0xD8);
    observeAllVolume(7, 3, 4, 1, 0xEA, true, 0xD6);
    at(1630);
    TEST_ASSERT_TRUE(statusContains("volume_timeout"));
}

void test_all_volume_response_arriving_before_new_read_request_is_not_fresh_evidence() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    at(100);
    observeAllVolume(7, 3, 4, 1); // delayed 0x3d before this operation's 0x3c
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

void test_pre_41037_volume_and_pre_41038_keep_current_send_zero_writes() {
    configureProfile();
    stageSnapshot(makeSnapshot(41036));
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("unsupported_firmware"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);

    setUp();
    configureProfile();
    profiles.loadableProfile.detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    profiles.loadableProfile.detector.displayPolicy = V1DisplayPolicy::Unchanged;
    profiles.loadableProfile.detector.modePolicy = V1ModePolicy::Unchanged;
    profiles.loadableProfile.detector.volumeDisconnect = V1VolumeDisconnectPolicy::KeepCurrent;
    stageSnapshot(makeSnapshot(41037));
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("unsupported_firmware"));
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

void test_main_display_off_can_keep_on_or_blinking_bluetooth_indicator_from_41032() {
    const V1BluetoothIndicatorState activeStates[] = {
        V1BluetoothIndicatorState::On, V1BluetoothIndicatorState::Blinking};
    for (const auto observedState : activeStates) {
        setUp();
        configureProfile();
        auto& detector = profiles.loadableProfile.detector;
        detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
        detector.modePolicy = V1ModePolicy::Unchanged;
        detector.volumePolicy = V1VolumePolicy::Unchanged;
        detector.bluetoothLedPolicy = V1BluetoothLedPolicy::On;
        stageSnapshot(makeSnapshot(41032));
        queueAndPreflight();
        at(100);
        TEST_ASSERT_EQUAL_INT(1, ble.setDisplayOnCalls);
        TEST_ASSERT_FALSE(ble.lastDisplayOnValue);
        TEST_ASSERT_TRUE(ble.lastKeepBluetoothIndicatorOn);
        observeDisplay(false, 1, 0xEA, false, 0xD8, UINT32_MAX, observedState);
        at(100);
        TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    }
}

void test_legacy_display_off_implicitly_turns_bluetooth_off_but_keep_on_is_gated() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    detector.bluetoothLedPolicy = V1BluetoothLedPolicy::Off;
    auto snapshot = makeSnapshot(41031);
    snapshot.bluetoothIndicator = V1BluetoothIndicatorState::On;
    stageSnapshot(snapshot);
    queueAndPreflight();
    at(100);
    TEST_ASSERT_FALSE(ble.lastKeepBluetoothIndicatorOn);
    observeDisplay(false, 1, 0xEA, false, 0xD8, UINT32_MAX,
                   V1BluetoothIndicatorState::Off);
    at(100);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));

    setUp();
    configureProfile();
    auto& gated = profiles.loadableProfile.detector;
    gated.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    gated.modePolicy = V1ModePolicy::Unchanged;
    gated.volumePolicy = V1VolumePolicy::Unchanged;
    gated.bluetoothLedPolicy = V1BluetoothLedPolicy::On;
    stageSnapshot(makeSnapshot(41031));
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("unsupported_bluetooth_led"));
    TEST_ASSERT_EQUAL_INT(0, ble.setDisplayOnCalls);
}

void test_bluetooth_policy_requires_final_main_display_off() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::On;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    detector.bluetoothLedPolicy = V1BluetoothLedPolicy::Off;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("invalid_policy"));
    TEST_ASSERT_EQUAL_INT(0, ble.setDisplayOnCalls);
}

void test_sparse_custom_definitions_write_used_only_commit_last_and_report_calibrated_readback() {
    const std::vector<V1CustomFrequencyDefinition> desired = {
        {0, 24200, 24300}, {1, 0, 0}, {2, 34500, 34600}, {3, 0, 0}};
    configureCustomOnly(desired);
    auto snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();

    at(100);
    TEST_ASSERT_EQUAL_INT(1, ble.writeSweepDefinitionCalls);
    at(105);
    TEST_ASSERT_EQUAL_INT(2, ble.writeSweepDefinitionCalls);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(ble.sweepWriteHistory.size()));
    TEST_ASSERT_EQUAL_UINT8(0, ble.sweepWriteHistory[0].index);
    TEST_ASSERT_FALSE(ble.sweepWriteHistory[0].commit);
    TEST_ASSERT_EQUAL_UINT8(2, ble.sweepWriteHistory[1].index);
    TEST_ASSERT_TRUE(ble.sweepWriteHistory[1].commit);
    observeSweepWriteResult(0);
    at(105);
    at(135);
    TEST_ASSERT_EQUAL_INT(1, ble.requestAllSweepDefinitionsCalls);
    observeSweepDefinition(0, 23950, 24950); // V1-calibrated, intentionally far from request
    observeSweepDefinition(1, 0, 0);
    observeSweepDefinition(2, 33100, 36900);
    observeSweepDefinition(3, 0, 0);
    at(135);

    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    JsonDocument status;
    const String json = module.getStatusJson();
    TEST_ASSERT_FALSE(deserializeJson(status, json.c_str()));
    JsonObject custom = status["components"]["customFrequencies"];
    TEST_ASSERT_TRUE(custom["verified"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("topology_and_live_section_with_calibrated_readback",
                             custom["verificationScope"].as<const char*>());
    TEST_ASSERT_EQUAL_UINT32(4, custom["requestedDefinitions"].size());
    TEST_ASSERT_EQUAL_UINT16(24200, custom["requestedDefinitions"][0]["lowerMHz"].as<uint16_t>());
    TEST_ASSERT_EQUAL_UINT16(23950, custom["calibratedReadback"][0]["lowerMHz"].as<uint16_t>());
    TEST_ASSERT_EQUAL_UINT16(33100, custom["calibratedReadback"][2]["lowerMHz"].as<uint16_t>());
}

void test_compact_authored_ranges_fill_live_table_and_disable_omitted_slots() {
    const std::vector<V1CustomFrequencyDefinition> desired = {
        {0, 24200, 24300}, {1, 34500, 34600}};
    configureCustomOnly(desired);
    auto snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();

    at(100);
    TEST_ASSERT_EQUAL_INT(1, ble.writeSweepDefinitionCalls);
    TEST_ASSERT_EQUAL_UINT8(0, ble.sweepWriteHistory[0].index);
    TEST_ASSERT_FALSE(ble.sweepWriteHistory[0].commit);
    at(105);
    TEST_ASSERT_EQUAL_INT(2, ble.writeSweepDefinitionCalls);
    TEST_ASSERT_EQUAL_UINT8(1, ble.sweepWriteHistory[1].index);
    TEST_ASSERT_TRUE(ble.sweepWriteHistory[1].commit);
    observeSweepWriteResult(0);
    at(105);
    at(135);
    observeSweepDefinition(0, 24195, 24305);
    observeSweepDefinition(1, 34495, 34605);
    observeSweepDefinition(2, 0, 0);
    observeSweepDefinition(3, 0, 0);
    at(135);

    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    JsonDocument status;
    const String json = module.getStatusJson();
    TEST_ASSERT_FALSE(deserializeJson(status, json.c_str()));
    JsonObject custom = status["components"]["customFrequencies"];
    TEST_ASSERT_EQUAL_UINT32(2, custom["requestedDefinitions"].size());
    TEST_ASSERT_EQUAL_UINT32(4, custom["calibratedReadback"].size());
    TEST_ASSERT_EQUAL_UINT16(0, custom["calibratedReadback"][2]["lowerMHz"].as<uint16_t>());
}

void test_exact_custom_definition_set_is_unchanged_and_sends_no_sweep_packets() {
    const std::vector<V1CustomFrequencyDefinition> desired = {
        {0, 24000, 24100}, {1, 0, 0}, {2, 34000, 34100}, {3, 0, 0}};
    configureCustomOnly(desired);
    auto snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();
    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_TRUE(statusContains("\"outcome\":\"unchanged\""));
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.requestAllSweepDefinitionsCalls);
}

void test_null_sweep_slots_round_trip_but_cannot_substitute_for_required_band_topology() {
    const std::vector<V1CustomFrequencyDefinition> desired = {
        {0, 24000, 24100}, {1, 0, 0}, {2, 34000, 34100}, {3, 0, 0}};
    configureCustomOnly(desired);
    auto snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    snapshot.sweepSectionCount = 3;
    snapshot.sweepSections = {{{0, 3, 23900, 25000}, {1, 3, 0, 0}, {2, 3, 33000, 37000}}};
    stageSnapshot(snapshot);
    queueAndPreflight();
    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_TRUE(statusContains("\"outcome\":\"unchanged\""));
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);

    setUp();
    configureCustomOnly(desired);
    snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    snapshot.sweepSections = {{{0, 2, 0, 0}, {1, 2, 0, 0}}};
    stageSnapshot(snapshot);
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("custom_configuration_invalid"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
}

void test_custom_commit_result_and_full_readback_are_both_required() {
    const std::vector<V1CustomFrequencyDefinition> desired = {
        {0, 24200, 24300}, {1, 0, 0}, {2, 34500, 34600}, {3, 0, 0}};
    configureCustomOnly(desired);
    auto snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();
    at(100);
    at(105);
    observeSweepWriteResult(1);
    at(105);
    TEST_ASSERT_TRUE(statusContains("custom_commit_rejected"));
    TEST_ASSERT_TRUE(statusContains("\"commitResultRaw\":1"));
    TEST_ASSERT_TRUE(statusContains("\"invalidDefinitionIndex\":0"));
    TEST_ASSERT_EQUAL_INT(0, ble.requestAllSweepDefinitionsCalls);

    setUp();
    configureCustomOnly(desired);
    snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();
    at(100);
    at(105);
    observeSweepWriteResult(64);
    at(105);
    TEST_ASSERT_TRUE(statusContains("\"commitResultRaw\":64"));
    TEST_ASSERT_TRUE(statusContains("\"invalidDefinitionIndex\":63"));

    setUp();
    configureCustomOnly(desired);
    snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();
    at(100);
    observeSweepWriteResult(0); // stale before the actual commit packet
    at(105);
    at(1605);
    TEST_ASSERT_TRUE(statusContains("custom_commit_timeout"));
}

void test_custom_readback_rejects_lost_used_range_and_cross_section_calibration() {
    const std::vector<V1CustomFrequencyDefinition> desired = {
        {0, 24200, 24300}, {1, 0, 0}, {2, 34500, 34600}, {3, 0, 0}};
    configureCustomOnly(desired);
    auto snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();
    at(100);
    at(105);
    observeSweepWriteResult(0);
    at(105);
    at(135);
    observeSweepDefinition(0, 0, 0); // lost write cannot masquerade as calibrated
    observeSweepDefinition(1, 0, 0);
    observeSweepDefinition(2, 34500, 34600);
    observeSweepDefinition(3, 0, 0);
    at(135);
    TEST_ASSERT_TRUE(statusContains("custom_readback_invalid"));

    setUp();
    configureCustomOnly(desired);
    snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();
    at(100);
    at(105);
    observeSweepWriteResult(0);
    at(105);
    at(135);
    observeSweepDefinition(0, 34000, 34100); // wrong live section
    observeSweepDefinition(1, 0, 0);
    observeSweepDefinition(2, 34500, 34600);
    observeSweepDefinition(3, 0, 0);
    at(135);
    TEST_ASSERT_TRUE(statusContains("custom_readback_invalid"));
}

void test_custom_readback_requires_every_definition_after_request_boundary() {
    const std::vector<V1CustomFrequencyDefinition> desired = {
        {0, 24200, 24300}, {1, 0, 0}, {2, 34500, 34600}, {3, 0, 0}};
    configureCustomOnly(desired);
    auto snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();
    at(100);
    at(105);
    observeSweepWriteResult(0);
    at(105);

    // This notification entered before the read request but remained queued
    // until after the request-side collector reset. Fresh peers must not make
    // its aggregate look current.
    const uint32_t queuedBeforeRequest = ble.latestV1NotificationIngressSequence();
    at(135);
    observeSweepDefinition(3, 0, 0, queuedBeforeRequest);
    observeSweepDefinition(0, 23950, 24950);
    observeSweepDefinition(1, 0, 0);
    observeSweepDefinition(2, 33100, 36900);
    at(135);
    TEST_ASSERT_TRUE(module.isActive());
    TEST_ASSERT_FALSE(statusContains("\"result\":\"succeeded\""));

    observeSweepDefinition(3, 0, 0);
    at(145);
    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
}

void test_custom_snapshot_mutation_before_preflight_blocks_every_write() {
    const std::vector<V1CustomFrequencyDefinition> desired = {
        {0, 24200, 24300}, {1, 0, 0}, {2, 34500, 34600}, {3, 0, 0}};
    configureCustomOnly(desired);
    auto snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    observeSweepDefinition(0, 24200, 24300, UINT32_MAX, false, 0xD6,
                           true); // contradictory newer canonical evidence poisons baseline
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("missing_live_snapshot"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
}

void test_malformed_canonical_sweep_definition_poison_blocks_every_write() {
    const std::vector<V1CustomFrequencyDefinition> desired = {
        {0, 24200, 24300}, {1, 0, 0}, {2, 34500, 34600}, {3, 0, 0}};
    configureCustomOnly(desired);
    auto snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);

    const auto malformed = makeV1Packet(
        PACKET_ID_RESP_SWEEP_DEFINITION,
        {0x80, 0x5E, 0x56, 0x00, 0x00});
    TEST_ASSERT_FALSE(parser.parse(malformed.data(), malformed.size(), mockMillis,
                                   ble.noteV1NotificationIngress()));
    TEST_ASSERT_TRUE(parser.sweepDefinitionsObservation().poisoned);
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("missing_live_snapshot"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
}

void test_poisoned_sweep_max_before_preflight_blocks_every_write() {
    const std::vector<V1CustomFrequencyDefinition> desired = {
        {0, 24200, 24300}, {1, 0, 0}, {2, 34500, 34600}, {3, 0, 0}};
    configureCustomOnly(desired);
    auto snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    const auto conflictingMax = makeV1Packet(PACKET_ID_RESP_MAX_SWEEP_INDEX,
                                             {static_cast<uint8_t>(snapshot.maxSweepIndex - 1u)});
    TEST_ASSERT_FALSE(parser.parse(conflictingMax.data(), conflictingMax.size(), mockMillis,
                                   ble.noteV1NotificationIngress()));
    TEST_ASSERT_TRUE(parser.sweepMaxObservation().poisoned);
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("missing_live_snapshot"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
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

void test_euro_bit_change_without_owned_definitions_accepts_detector_factory_reset() {
    const std::array<uint8_t, 6> before{{0xFF, 0xFF, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> desired{{0xFF, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureProfile(desired);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot(41039, before));
    queueAndPreflight();
    at(100);
    at(130);
    observeUserBytes(ble.lastUserBytes);
    at(130);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_EQUAL_INT(1, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
}

void test_euro_bit_change_restores_explicit_custom_definitions_after_user_write() {
    const std::array<uint8_t, 6> before{{0xFF, 0xFF, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> desiredBytes{{0xFF, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureProfile(desiredBytes);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    const std::array<V1CustomFrequencyDefinition, 4> euroDefinitions{{
        {0, 24200, 24300}, {1, 0, 0}, {2, 34500, 34600}, {3, 0, 0}}};
    TEST_ASSERT_TRUE(detector.customFrequencyDefinitions.assign(euroDefinitions));
    auto snapshot = makeSnapshot(41039, before);
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();
    at(100); // user write
    at(130); // user read
    observeUserBytes(ble.lastUserBytes);
    at(130); // user verify; target-region recapture scheduled
    at(160); // sweep sections request
    at(190); // max index request
    at(220); // complete definitions request
    auto target = makeSnapshot(41039, desiredBytes);
    addSweepSnapshot(target);
    target.sweepDefinitions = {{{0, 24050, 24150}, {1, 0, 0},
                                {2, 34100, 34200}, {3, 0, 0}}};
    completeTargetRegionSweepRefresh(target, 220);
    at(250); // first used definition
    at(255); // final used definition and commit
    observeSweepWriteResult(0);
    at(255);
    at(285);
    observeSweepDefinition(0, 24195, 24305);
    observeSweepDefinition(1, 0, 0);
    observeSweepDefinition(2, 34495, 34605);
    observeSweepDefinition(3, 0, 0);
    at(285);

    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_EQUAL_UINT32(7, static_cast<uint32_t>(ble.commandHistory.size()));
    TEST_ASSERT_EQUAL_STRING("user-write", ble.commandHistory[0]);
    TEST_ASSERT_EQUAL_STRING("sweep-sections-read", ble.commandHistory[1]);
    TEST_ASSERT_EQUAL_STRING("sweep-max-read", ble.commandHistory[2]);
    TEST_ASSERT_EQUAL_STRING("sweep-read", ble.commandHistory[3]);
    TEST_ASSERT_EQUAL_STRING("sweep-write", ble.commandHistory[4]);
    TEST_ASSERT_EQUAL_STRING("sweep-commit", ble.commandHistory[5]);
    TEST_ASSERT_EQUAL_STRING("sweep-read", ble.commandHistory[6]);
}

void test_usa_to_euro_from_advanced_requires_explicit_non_advanced_mode_before_writes() {
    const std::array<uint8_t, 6> before{{0xFF, 0xFF, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> desiredBytes{{0xFF, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureProfile(desiredBytes);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    const std::array<V1CustomFrequencyDefinition, 4> euroDefinitions{{
        {0, 24200, 24300}, {1, 0, 0}, {2, 34500, 34600}, {3, 0, 0}}};
    TEST_ASSERT_TRUE(detector.customFrequencyDefinitions.assign(euroDefinitions));
    auto snapshot = makeSnapshot(41039, before);
    snapshot.mode = 'L';
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("euro_advanced_mode_invalid"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
}

void test_enabling_custom_filtering_without_profile_owned_ranges_is_rejected() {
    const std::array<uint8_t, 6> before{{0xFF, 0xFF, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> enableCustom{{0xFF, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureProfile(enableCustom);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot(41039, before));
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("custom_configuration_invalid"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.requestAllSweepDefinitionsCalls);
}

void test_enabling_custom_filtering_syncs_profile_owned_compact_ranges() {
    const std::array<uint8_t, 6> before{{0xFF, 0xFF, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> enableCustom{{0xFF, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureProfile(enableCustom);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    const std::array<V1CustomFrequencyDefinition, 2> ranges{{
        {0, 24200, 24300}, {1, 34500, 34600}}};
    TEST_ASSERT_TRUE(detector.customFrequencyDefinitions.assign(ranges));
    auto snapshot = makeSnapshot(41039, before);
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();

    at(100); // first sweep; the final enable byte has not been sent
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    at(105); // final sweep and commit
    observeSweepWriteResult(0);
    at(105);
    at(135); // focused full-table readback request
    observeSweepDefinition(0, 24195, 24305);
    observeSweepDefinition(1, 34495, 34605);
    observeSweepDefinition(2, 0, 0);
    observeSweepDefinition(3, 0, 0);
    at(135); // custom verify; final user write is now scheduled
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    at(165); // final user write enables Custom Frequencies
    TEST_ASSERT_EQUAL_HEX8(0xF7, ble.lastUserBytes[1]);
    at(195); // focused user-byte read
    observeUserBytes(ble.lastUserBytes);
    at(195);

    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_EQUAL_INT(1, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(2, ble.writeSweepDefinitionCalls);
    TEST_ASSERT_EQUAL_INT(1, ble.requestAllSweepDefinitionsCalls);
    TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(ble.commandHistory.size()));
    TEST_ASSERT_EQUAL_STRING("sweep-write", ble.commandHistory[0]);
    TEST_ASSERT_EQUAL_STRING("sweep-commit", ble.commandHistory[1]);
    TEST_ASSERT_EQUAL_STRING("sweep-read", ble.commandHistory[2]);
    TEST_ASSERT_EQUAL_STRING("user-write", ble.commandHistory[3]);
}

void test_enabling_custom_recommits_an_exact_complete_table_before_enable() {
    const std::array<uint8_t, 6> before{{0xFF, 0xFF, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> enableCustom{{0xFF, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureProfile(enableCustom);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    const std::array<V1CustomFrequencyDefinition, 4> exact{{
        {0, 24000, 24100}, {1, 0, 0}, {2, 34000, 34100}, {3, 0, 0}}};
    TEST_ASSERT_TRUE(detector.customFrequencyDefinitions.assign(exact));
    auto snapshot = makeSnapshot(41039, before);
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();

    at(100);
    at(105);
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    observeSweepWriteResult(0);
    at(105);
    at(135);
    observeSweepDefinition(0, 24000, 24100);
    observeSweepDefinition(1, 0, 0);
    observeSweepDefinition(2, 34000, 34100);
    observeSweepDefinition(3, 0, 0);
    at(135);
    at(165);
    at(195);
    observeUserBytes(ble.lastUserBytes);
    at(195);

    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_EQUAL_INT(2, ble.writeSweepDefinitionCalls);
    TEST_ASSERT_EQUAL_INT(1, ble.requestAllSweepDefinitionsCalls);
    TEST_ASSERT_EQUAL_INT(1, ble.writeUserBytesCalls);
}

void test_region_change_with_custom_on_uses_verified_disabled_intermediate_then_table_then_final_enable() {
    const std::array<uint8_t, 6> before{{0xFF, 0xF7, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> euroCustomOn{{0xFF, 0xF6, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureProfile(euroCustomOn);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    const std::array<V1CustomFrequencyDefinition, 2> ranges{{
        {0, 24200, 24300}, {1, 34500, 34600}}};
    TEST_ASSERT_TRUE(detector.customFrequencyDefinitions.assign(ranges));
    auto snapshot = makeSnapshot(41039, before);
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();

    at(100); // target region, Custom Frequencies forced off
    TEST_ASSERT_EQUAL_HEX8(0xFE, ble.lastUserBytes[1]);
    at(130);
    observeUserBytes(ble.lastUserBytes);
    at(130);
    TEST_ASSERT_TRUE(statusContains("\"step\":\"CustomRefreshSections\""));
    TEST_ASSERT_TRUE(statusContains("\"profile\":{\"requested\":true,\"beforeAvailable\":true,\"needed\":true,\"sent\":true,\"verified\":false"));
    at(160);
    at(190);
    at(220);
    auto target = makeSnapshot(41039, euroCustomOn);
    addSweepSnapshot(target);
    target.sweepDefinitions = {{{0, 24050, 24150}, {1, 0, 0},
                                {2, 34100, 34200}, {3, 0, 0}}};
    completeTargetRegionSweepRefresh(target, 220);
    at(250);
    at(255);
    observeSweepWriteResult(0);
    at(255);
    at(285);
    observeSweepDefinition(0, 24195, 24305);
    observeSweepDefinition(1, 34495, 34605);
    observeSweepDefinition(2, 0, 0);
    observeSweepDefinition(3, 0, 0);
    at(285);
    TEST_ASSERT_EQUAL_INT(1, ble.writeUserBytesCalls);
    at(315); // final target-region bytes enable Custom Frequencies
    TEST_ASSERT_EQUAL_HEX8(0xF6, ble.lastUserBytes[1]);
    at(345);
    observeUserBytes(ble.lastUserBytes);
    at(345);

    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_EQUAL_INT(2, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_UINT32(8, static_cast<uint32_t>(ble.commandHistory.size()));
    TEST_ASSERT_EQUAL_STRING("user-write", ble.commandHistory[0]);
    TEST_ASSERT_EQUAL_STRING("sweep-sections-read", ble.commandHistory[1]);
    TEST_ASSERT_EQUAL_STRING("sweep-max-read", ble.commandHistory[2]);
    TEST_ASSERT_EQUAL_STRING("sweep-read", ble.commandHistory[3]);
    TEST_ASSERT_EQUAL_STRING("sweep-write", ble.commandHistory[4]);
    TEST_ASSERT_EQUAL_STRING("sweep-commit", ble.commandHistory[5]);
    TEST_ASSERT_EQUAL_STRING("sweep-read", ble.commandHistory[6]);
    TEST_ASSERT_EQUAL_STRING("user-write", ble.commandHistory[7]);
}

void test_region_change_k_only_uses_fresh_target_ka_not_source_region_ka() {
    const std::array<uint8_t, 6> before{{0xFF, 0xF7, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> euroCustomOn{{0xFF, 0xF6, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureProfile(euroCustomOn);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    TEST_ASSERT_TRUE(detector.customFrequencyDefinitions.assign(
        std::array<V1CustomFrequencyDefinition, 1>{{{0, 24200, 24300}}}));
    auto source = makeSnapshot(41039, before);
    addSweepSnapshot(source); // source Ka is 34000-34100
    stageSnapshot(source);
    queueAndPreflight();

    at(100);
    at(130);
    observeUserBytes(ble.lastUserBytes);
    at(130);
    at(160);
    at(190);
    at(220);
    auto target = makeSnapshot(41039, euroCustomOn);
    addSweepSnapshot(target);
    target.sweepDefinitions = {{{0, 24050, 24150}, {1, 0, 0},
                                {2, 35000, 35100}, {3, 0, 0}}};
    completeTargetRegionSweepRefresh(target, 220);

    at(250);
    TEST_ASSERT_EQUAL_UINT8(0, ble.sweepWriteHistory[0].index);
    TEST_ASSERT_EQUAL_UINT16(24200, ble.sweepWriteHistory[0].lower);
    at(255);
    TEST_ASSERT_EQUAL_UINT8(2, ble.sweepWriteHistory[1].index);
    TEST_ASSERT_EQUAL_UINT16(35000, ble.sweepWriteHistory[1].lower);
    TEST_ASSERT_EQUAL_UINT16(35100, ble.sweepWriteHistory[1].upper);
    TEST_ASSERT_TRUE(ble.sweepWriteHistory[1].commit);
    observeSweepWriteResult(0);
    at(255);
    at(285);
    observeSweepDefinition(0, 24195, 24305);
    observeSweepDefinition(1, 0, 0);
    observeSweepDefinition(2, 35000, 35100);
    observeSweepDefinition(3, 0, 0);
    at(285);
    at(315);
    at(345);
    observeUserBytes(ble.lastUserBytes);
    at(345);

    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_EQUAL_HEX8(0xF6, ble.lastUserBytes[1]);
}

void test_region_partial_ownership_fails_closed_on_incomplete_target_refresh() {
    const std::array<uint8_t, 6> before{{0xFF, 0xF7, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> euroCustomOn{{0xFF, 0xF6, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureProfile(euroCustomOn);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    TEST_ASSERT_TRUE(detector.customFrequencyDefinitions.assign(
        std::array<V1CustomFrequencyDefinition, 1>{{{0, 24200, 24300}}}));
    auto source = makeSnapshot(41039, before);
    addSweepSnapshot(source);
    stageSnapshot(source);
    queueAndPreflight();

    at(100);
    at(130);
    observeUserBytes(ble.lastUserBytes);
    at(130);
    at(160);
    at(190);
    at(220);
    auto target = makeSnapshot(41039, euroCustomOn);
    addSweepSnapshot(target);
    observeCapturedSweepSections(target);
    observeCapturedSweepMax(target.maxSweepIndex);
    at(1720);

    TEST_ASSERT_TRUE(statusContains("custom_readback_timeout"));
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
    TEST_ASSERT_EQUAL_INT(1, ble.writeUserBytesCalls);
}

void test_region_refresh_request_failures_never_reach_table_or_final_enable() {
    const auto prepare = []() {
        const std::array<uint8_t, 6> before{{0xFF, 0xF7, 0xFF, 0xFF, 0xA4, 0x5A}};
        const std::array<uint8_t, 6> euroCustomOn{{0xFF, 0xF6, 0xFF, 0xFF, 0xFF, 0xFF}};
        configureProfile(euroCustomOn);
        auto& detector = profiles.loadableProfile.detector;
        detector.displayPolicy = V1DisplayPolicy::Unchanged;
        detector.modePolicy = V1ModePolicy::Unchanged;
        detector.volumePolicy = V1VolumePolicy::Unchanged;
        detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
        TEST_ASSERT_TRUE(detector.customFrequencyDefinitions.assign(
            std::array<V1CustomFrequencyDefinition, 1>{{{0, 24200, 24300}}}));
        auto source = makeSnapshot(41039, before);
        addSweepSnapshot(source);
        stageSnapshot(source);
        queueAndPreflight();
        at(100);
        at(130);
        observeUserBytes(ble.lastUserBytes);
        at(130);
    };
    const auto assertStopped = []() {
        TEST_ASSERT_TRUE(statusContains("custom_read_failed"));
        TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
        TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
        TEST_ASSERT_EQUAL_INT(1, ble.writeUserBytesCalls);
    };

    prepare();
    ble.nextRequestSweepSectionsResult = SendResult::FAILED;
    at(160);
    assertStopped();

    setUp();
    prepare();
    at(160);
    ble.nextRequestMaxSweepIndexResult = SendResult::FAILED;
    at(190);
    assertStopped();

    setUp();
    prepare();
    at(160);
    at(190);
    ble.nextRequestAllSweepDefinitionsResult = SendResult::FAILED;
    at(220);
    assertStopped();
}

void test_region_refresh_not_yet_retries_each_request_without_failing_apply() {
    const std::array<uint8_t, 6> before{{0xFF, 0xF7, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> euroCustomOn{{0xFF, 0xF6, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureProfile(euroCustomOn);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    TEST_ASSERT_TRUE(detector.customFrequencyDefinitions.assign(
        std::array<V1CustomFrequencyDefinition, 1>{{{0, 24200, 24300}}}));
    auto source = makeSnapshot(41039, before);
    addSweepSnapshot(source);
    stageSnapshot(source);
    queueAndPreflight();
    at(100);
    at(130);
    observeUserBytes(ble.lastUserBytes);
    at(130);

    ble.nextRequestSweepSectionsResult = SendResult::NOT_YET;
    at(160);
    TEST_ASSERT_TRUE(module.isActive());
    TEST_ASSERT_FALSE(statusContains("custom_read_failed"));
    ble.nextRequestSweepSectionsResult = SendResult::SENT;
    at(165);

    ble.nextRequestMaxSweepIndexResult = SendResult::NOT_YET;
    at(195);
    TEST_ASSERT_TRUE(module.isActive());
    TEST_ASSERT_FALSE(statusContains("custom_read_failed"));
    ble.nextRequestMaxSweepIndexResult = SendResult::SENT;
    at(200);

    ble.nextRequestAllSweepDefinitionsResult = SendResult::NOT_YET;
    at(230);
    TEST_ASSERT_TRUE(module.isActive());
    TEST_ASSERT_FALSE(statusContains("custom_read_failed"));
    ble.nextRequestAllSweepDefinitionsResult = SendResult::SENT;
    at(235);

    auto target = makeSnapshot(41039, euroCustomOn);
    addSweepSnapshot(target);
    target.sweepDefinitions = {{{0, 24050, 24150}, {1, 0, 0},
                                {2, 34100, 34200}, {3, 0, 0}}};
    completeTargetRegionSweepRefresh(target, 235);

    TEST_ASSERT_TRUE(module.isActive());
    TEST_ASSERT_FALSE(statusContains("custom_read_failed"));
    TEST_ASSERT_EQUAL_INT(2, ble.requestSweepSectionsCalls);
    TEST_ASSERT_EQUAL_INT(2, ble.requestMaxSweepIndexCalls);
    TEST_ASSERT_EQUAL_INT(2, ble.requestAllSweepDefinitionsCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
    at(265);
    TEST_ASSERT_EQUAL_INT(1, ble.writeSweepDefinitionCalls);
}

void test_region_refresh_not_yet_has_bounded_send_deadline() {
    const std::array<uint8_t, 6> before{{0xFF, 0xF7, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> euroCustomOn{{0xFF, 0xF6, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureProfile(euroCustomOn);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    TEST_ASSERT_TRUE(detector.customFrequencyDefinitions.assign(
        std::array<V1CustomFrequencyDefinition, 1>{{{0, 24200, 24300}}}));
    auto source = makeSnapshot(41039, before);
    addSweepSnapshot(source);
    stageSnapshot(source);
    queueAndPreflight();
    at(100);
    at(130);
    observeUserBytes(ble.lastUserBytes);
    at(130);

    ble.nextRequestSweepSectionsResult = SendResult::NOT_YET;
    at(160);
    TEST_ASSERT_TRUE(module.isActive());
    at(1660);

    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_TRUE(statusContains("custom_readback_timeout"));
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
    TEST_ASSERT_EQUAL_INT(2, ble.requestSweepSectionsCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.requestMaxSweepIndexCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
}

void test_custom_readback_request_not_yet_retries_while_failed_is_terminal() {
    const std::vector<V1CustomFrequencyDefinition> desired = {
        {0, 24200, 24300}, {1, 0, 0}, {2, 34500, 34600}, {3, 0, 0}};
    configureCustomOnly(desired);
    auto snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();
    at(100);
    at(105);
    observeSweepWriteResult(0);
    at(105);

    ble.nextRequestAllSweepDefinitionsResult = SendResult::NOT_YET;
    at(135);
    TEST_ASSERT_TRUE(module.isActive());
    TEST_ASSERT_FALSE(statusContains("custom_read_failed"));
    ble.nextRequestAllSweepDefinitionsResult = SendResult::SENT;
    at(140);
    observeSweepDefinition(0, 23950, 24950);
    observeSweepDefinition(1, 0, 0);
    observeSweepDefinition(2, 33100, 36900);
    observeSweepDefinition(3, 0, 0);
    at(140);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_EQUAL_INT(2, ble.requestAllSweepDefinitionsCalls);

    setUp();
    configureCustomOnly(desired);
    snapshot = makeSnapshot();
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();
    at(100);
    at(105);
    observeSweepWriteResult(0);
    at(105);
    ble.nextRequestAllSweepDefinitionsResult = SendResult::FAILED;
    at(135);

    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_TRUE(statusContains("custom_read_failed"));
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
    TEST_ASSERT_EQUAL_INT(1, ble.requestAllSweepDefinitionsCalls);
}

void test_region_refresh_poisoned_definition_fails_before_table_or_final_enable() {
    const std::array<uint8_t, 6> before{{0xFF, 0xF7, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> euroCustomOn{{0xFF, 0xF6, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureProfile(euroCustomOn);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    TEST_ASSERT_TRUE(detector.customFrequencyDefinitions.assign(
        std::array<V1CustomFrequencyDefinition, 1>{{{0, 24200, 24300}}}));
    auto source = makeSnapshot(41039, before);
    addSweepSnapshot(source);
    stageSnapshot(source);
    queueAndPreflight();
    at(100);
    at(130);
    observeUserBytes(ble.lastUserBytes);
    at(130);
    at(160);
    at(190);
    at(220);

    auto target = makeSnapshot(41039, euroCustomOn);
    addSweepSnapshot(target);
    observeCapturedSweepSections(target);
    observeCapturedSweepMax(target.maxSweepIndex);
    observeCapturedSweepDefinition(0, 24050, 24150);
    observeSweepDefinition(0, 24060, 24160, UINT32_MAX, false, 0xD6,
                           true); // same fresh index, conflicting target evidence
    at(220);

    TEST_ASSERT_TRUE(statusContains("custom_readback_invalid"));
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
    TEST_ASSERT_EQUAL_INT(1, ble.writeUserBytesCalls);
}

void test_region_custom_on_disconnects_never_report_success_at_any_transaction_phase() {
    const auto prepare = []() {
        const std::array<uint8_t, 6> before{{0xFF, 0xF7, 0xFF, 0xFF, 0xA4, 0x5A}};
        const std::array<uint8_t, 6> euroCustomOn{{0xFF, 0xF6, 0xFF, 0xFF, 0xFF, 0xFF}};
        configureProfile(euroCustomOn);
        auto& detector = profiles.loadableProfile.detector;
        detector.displayPolicy = V1DisplayPolicy::Unchanged;
        detector.modePolicy = V1ModePolicy::Unchanged;
        detector.volumePolicy = V1VolumePolicy::Unchanged;
        detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
        const std::array<V1CustomFrequencyDefinition, 2> ranges{{
            {0, 24200, 24300}, {1, 34500, 34600}}};
        TEST_ASSERT_TRUE(detector.customFrequencyDefinitions.assign(ranges));
        auto snapshot = makeSnapshot(41039, before);
        addSweepSnapshot(snapshot);
        stageSnapshot(snapshot);
        queueAndPreflight();
    };
    const auto verifyIntermediate = []() {
        at(100);
        at(130);
        observeUserBytes(ble.lastUserBytes);
        at(130);
    };
    const auto verifyCustomTable = []() {
        at(160);
        at(190);
        at(220);
        auto target = makeSnapshot(41039,
                                   {{0xFF, 0xFE, 0xFF, 0xFF, 0xA4, 0x5A}});
        addSweepSnapshot(target);
        target.sweepDefinitions = {{{0, 24050, 24150}, {1, 0, 0},
                                    {2, 34100, 34200}, {3, 0, 0}}};
        completeTargetRegionSweepRefresh(target, 220);
        at(250);
        at(255);
        observeSweepWriteResult(0);
        at(255);
        at(285);
        observeSweepDefinition(0, 24195, 24305);
        observeSweepDefinition(1, 34495, 34605);
        observeSweepDefinition(2, 0, 0);
        observeSweepDefinition(3, 0, 0);
        at(285);
    };

    prepare();
    at(100); // intermediate write sent, not verified
    ble.setConnected(false);
    at(130);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
    TEST_ASSERT_FALSE(statusContains("\"result\":\"succeeded\""));

    setUp();
    prepare();
    verifyIntermediate();
    ble.setConnected(false); // intermediate verified, table not started
    at(160);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);

    setUp();
    prepare();
    verifyIntermediate();
    at(160);
    ble.setConnected(false); // sections requested, target refresh incomplete
    at(190);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);

    setUp();
    prepare();
    verifyIntermediate();
    at(160);
    at(190);
    ble.setConnected(false); // max requested, target refresh incomplete
    at(220);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);

    setUp();
    prepare();
    verifyIntermediate();
    at(160);
    at(190);
    at(220);
    ble.setConnected(false); // definitions requested, no complete target evidence
    at(220);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);

    setUp();
    prepare();
    verifyIntermediate();
    at(160);
    at(190);
    at(220);
    auto target = makeSnapshot(41039,
                               {{0xFF, 0xFE, 0xFF, 0xFF, 0xA4, 0x5A}});
    addSweepSnapshot(target);
    completeTargetRegionSweepRefresh(target, 220);
    at(250);
    at(255); // final sweep and commit sent, result not verified
    ble.setConnected(false);
    at(255);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
    TEST_ASSERT_FALSE(statusContains("\"result\":\"succeeded\""));

    setUp();
    prepare();
    verifyIntermediate();
    verifyCustomTable();
    ble.setConnected(false); // full table verified, final enable not sent
    at(315);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"partial\""));
    TEST_ASSERT_EQUAL_INT(1, ble.writeUserBytesCalls);
    TEST_ASSERT_FALSE(statusContains("\"result\":\"succeeded\""));

    setUp();
    prepare();
    verifyIntermediate();
    verifyCustomTable();
    at(315); // final enable sent, readback not verified
    ble.setConnected(false);
    at(345);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"partial\""));
    TEST_ASSERT_EQUAL_INT(2, ble.writeUserBytesCalls);
    TEST_ASSERT_FALSE(statusContains("\"result\":\"succeeded\""));
}

void test_k_only_authorship_preserves_live_ka_when_compiled_table_is_unchanged() {
    const std::array<uint8_t, 6> enabled{{0xFF, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureCustomOnly({{0, 24000, 24100}, {1, 0, 0},
                         {2, 0, 0}, {3, 0, 0}});
    auto snapshot = makeSnapshot(41039, enabled);
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_TRUE(statusContains("\"outcome\":\"unchanged\""));
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
}

void test_k_only_authorship_replaces_k_and_copies_live_ka_without_invention() {
    const std::array<uint8_t, 6> enabled{{0xFF, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureCustomOnly({{0, 24200, 24300}});
    auto snapshot = makeSnapshot(41039, enabled);
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();

    at(100);
    TEST_ASSERT_EQUAL_UINT8(0, ble.sweepWriteHistory[0].index);
    TEST_ASSERT_EQUAL_UINT16(24200, ble.sweepWriteHistory[0].lower);
    at(105);
    TEST_ASSERT_EQUAL_UINT8(2, ble.sweepWriteHistory[1].index);
    TEST_ASSERT_EQUAL_UINT16(34000, ble.sweepWriteHistory[1].lower);
    TEST_ASSERT_EQUAL_UINT16(34100, ble.sweepWriteHistory[1].upper);
    TEST_ASSERT_TRUE(ble.sweepWriteHistory[1].commit);
    observeSweepWriteResult(0);
    at(105);
    at(135);
    observeSweepDefinition(0, 24195, 24305);
    observeSweepDefinition(1, 0, 0);
    observeSweepDefinition(2, 34000, 34100);
    observeSweepDefinition(3, 0, 0);
    at(135);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
}

void test_ka_only_authorship_relocates_around_and_preserves_live_k() {
    const std::array<uint8_t, 6> enabled{{0xFF, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureCustomOnly({{0, 34500, 34600}});
    auto snapshot = makeSnapshot(41039, enabled);
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();

    at(100);
    TEST_ASSERT_EQUAL_UINT8(0, ble.sweepWriteHistory[0].index);
    TEST_ASSERT_EQUAL_UINT16(24000, ble.sweepWriteHistory[0].lower);
    TEST_ASSERT_EQUAL_UINT16(24100, ble.sweepWriteHistory[0].upper);
    at(105);
    TEST_ASSERT_EQUAL_UINT8(1, ble.sweepWriteHistory[1].index);
    TEST_ASSERT_EQUAL_UINT16(34500, ble.sweepWriteHistory[1].lower);
    TEST_ASSERT_TRUE(ble.sweepWriteHistory[1].commit);
    observeSweepWriteResult(0);
    at(105);
    at(135);
    observeSweepDefinition(0, 24000, 24100);
    observeSweepDefinition(1, 34495, 34605);
    observeSweepDefinition(2, 0, 0);
    observeSweepDefinition(3, 0, 0);
    at(135);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
}

void test_unowned_band_readback_must_equal_the_fresh_live_definition() {
    const std::array<uint8_t, 6> enabled{{0xFF, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureCustomOnly({{0, 34500, 34600}});
    auto snapshot = makeSnapshot(41039, enabled);
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();

    at(100);
    at(105);
    observeSweepWriteResult(0);
    at(105);
    at(135);
    observeSweepDefinition(0, 24010, 24110); // still K, but not the preserved live range
    observeSweepDefinition(1, 34495, 34605);
    observeSweepDefinition(2, 0, 0);
    observeSweepDefinition(3, 0, 0);
    at(135);

    TEST_ASSERT_TRUE(statusContains("custom_readback_invalid"));
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
}

void test_no_authored_ranges_infers_no_owned_bands_and_preserves_live_table() {
    const std::array<uint8_t, 6> enabled{{0xFF, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureCustomOnly({});
    auto snapshot = makeSnapshot(41039, enabled);
    addSweepSnapshot(snapshot);
    stageSnapshot(snapshot);
    queueAndPreflight();

    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_TRUE(statusContains("\"outcome\":\"unchanged\""));
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
}

void test_enabling_band_while_custom_is_on_requires_profile_owned_ranges() {
    const std::array<uint8_t, 6> kOnlyBefore{{0xFB, 0xF7, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> kAndKaAfter{{0xFF, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureProfile(kAndKaAfter);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot(41039, kOnlyBefore));
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("custom_configuration_invalid"));
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.requestAllSweepDefinitionsCalls);
}

void test_disabling_custom_filtering_does_not_require_or_write_definitions() {
    const std::array<uint8_t, 6> enabled{{0xFF, 0xF7, 0xFF, 0xFF, 0xA4, 0x5A}};
    const std::array<uint8_t, 6> disabled{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    configureProfile(disabled);
    auto& detector = profiles.loadableProfile.detector;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot(41039, enabled)); // no sweep capture
    queueAndPreflight();
    at(100);
    at(130);
    observeUserBytes(ble.lastUserBytes);
    at(130);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_EQUAL_INT(0, ble.writeSweepDefinitionCalls);
}

void test_unrelated_display_apply_does_not_require_sweeps_when_custom_was_already_enabled() {
    const std::array<uint8_t, 6> enabled{{0xFF, 0xF7, 0xFF, 0xFF, 0xA4, 0x5A}};
    configureProfile(enabled);
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Unchanged;
    stageSnapshot(makeSnapshot(41039, enabled));
    queueAndPreflight();
    at(100);
    observeDisplay(false, 1);
    at(100);
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
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
    observeAllVolume(7, 3, 4, 1);
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
    at(130); // 0x3c request
    observeAllVolume(7, 3, 4, 1, 0xEA, false, 0xD6, queuedBeforeRequest);
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
    ble.requestAllVolumeSendHook = injectMatchingAllVolumeDuringSend;
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
    ble.requestAllVolumeResult = false;
    queueAndPreflight();
    at(100);
    at(130);
    TEST_ASSERT_TRUE(statusContains("volume_read_failed"));
    TEST_ASSERT_EQUAL_INT(SendResult::SENT,
                          quiet.sendVolumeResult(QuietOwner::VolumeFade, 1, 1));
    TEST_ASSERT_FALSE(ble.consumeVerifyPushMatchEdge());
}

void test_queue_failures_preserve_durable_admission_taxonomy() {
    using Queue = AutoPushModule::QueueResult;
    using Reason = V1SettingsOperationStore::Reason;
    TEST_ASSERT_EQUAL_INT(Reason::None, AutoPushModule::durableReasonForQueueResult(Queue::QUEUED));
    TEST_ASSERT_EQUAL_INT(Reason::DetectorDisconnected,
                          AutoPushModule::durableReasonForQueueResult(Queue::V1_NOT_CONNECTED));
    TEST_ASSERT_EQUAL_INT(Reason::ExecutorBusy,
                          AutoPushModule::durableReasonForQueueResult(Queue::ALREADY_IN_PROGRESS));
    TEST_ASSERT_EQUAL_INT(Reason::NoProfileConfigured,
                          AutoPushModule::durableReasonForQueueResult(Queue::NO_PROFILE_CONFIGURED));
    TEST_ASSERT_EQUAL_INT(Reason::ProfileBusy,
                          AutoPushModule::durableReasonForQueueResult(Queue::PROFILE_BUSY));
    TEST_ASSERT_EQUAL_INT(Reason::ProfileLoadFailed,
                          AutoPushModule::durableReasonForQueueResult(Queue::PROFILE_LOAD_FAILED));
    TEST_ASSERT_EQUAL_INT(Reason::InvalidConfiguration,
                          AutoPushModule::durableReasonForQueueResult(Queue::INVALID_VOLUME_PAIR));
    TEST_ASSERT_EQUAL_INT(Reason::UnsupportedConfiguration,
                          AutoPushModule::durableReasonForQueueResult(Queue::UNSUPPORTED_CONFIGURATION));
    TEST_ASSERT_EQUAL_INT(Reason::ActiveSlotPersistFailed,
                          AutoPushModule::durableReasonForQueueResult(Queue::ACTIVE_SLOT_PERSIST_FAILED));
    TEST_ASSERT_EQUAL_INT(Reason::StagingUnavailable,
                          AutoPushModule::durableReasonForQueueResult(Queue::STAGING_UNAVAILABLE));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_queue_rejects_failed_active_slot_persistence_before_operation_or_detector_write);
    RUN_TEST(test_queue_stages_every_owned_string_and_definition_capacity_before_activation);
    RUN_TEST(test_queue_secures_maximum_v3_profile_before_activation_and_preserves_legacy_load_step);
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
    RUN_TEST(test_rejected_display_interleaving_cannot_replace_canonical_evidence_value);
    RUN_TEST(test_fresh_display_mismatches_remain_pending_then_report_mismatch_at_deadline);
    RUN_TEST(test_mode_requires_fresh_canonical_display_evidence_and_reports_mismatch);
    RUN_TEST(test_temporary_volume_uses_all_volume_readback_and_preserves_saved_pair);
    RUN_TEST(test_saved_volume_sets_save_aux_and_verifies_both_current_and_saved_pairs);
    RUN_TEST(test_volume_behavior_aux_is_sent_even_when_values_already_match);
    RUN_TEST(test_volume_readback_mismatch_is_not_success);
    RUN_TEST(test_wrong_destination_and_corrupt_all_volume_responses_cannot_verify);
    RUN_TEST(test_all_volume_response_arriving_before_new_read_request_is_not_fresh_evidence);
    RUN_TEST(test_missing_display_before_state_fails_whole_plan_without_writes);
    RUN_TEST(test_missing_mode_and_volume_before_observations_each_fail_without_writes);
    RUN_TEST(test_snapshot_value_must_still_match_latest_canonical_observation_at_preflight);
    RUN_TEST(test_missing_user_before_fails_closed_to_preserve_region_and_unknown_bits);
    RUN_TEST(test_pre_41037_volume_and_pre_41038_keep_current_send_zero_writes);
    RUN_TEST(test_unsupported_old_firmware_preflight_sends_nothing);
    RUN_TEST(test_unverified_future_major_firmware_blocks_every_write);
    RUN_TEST(test_main_display_off_can_keep_on_or_blinking_bluetooth_indicator_from_41032);
    RUN_TEST(test_legacy_display_off_implicitly_turns_bluetooth_off_but_keep_on_is_gated);
    RUN_TEST(test_bluetooth_policy_requires_final_main_display_off);
    RUN_TEST(test_sparse_custom_definitions_write_used_only_commit_last_and_report_calibrated_readback);
    RUN_TEST(test_compact_authored_ranges_fill_live_table_and_disable_omitted_slots);
    RUN_TEST(test_exact_custom_definition_set_is_unchanged_and_sends_no_sweep_packets);
    RUN_TEST(test_null_sweep_slots_round_trip_but_cannot_substitute_for_required_band_topology);
    RUN_TEST(test_custom_commit_result_and_full_readback_are_both_required);
    RUN_TEST(test_custom_readback_rejects_lost_used_range_and_cross_section_calibration);
    RUN_TEST(test_custom_readback_requires_every_definition_after_request_boundary);
    RUN_TEST(test_custom_snapshot_mutation_before_preflight_blocks_every_write);
    RUN_TEST(test_malformed_canonical_sweep_definition_poison_blocks_every_write);
    RUN_TEST(test_poisoned_sweep_max_before_preflight_blocks_every_write);
    RUN_TEST(test_vendor_invalid_zero_multibit_user_values_fail_preflight_at_version_boundaries);
    RUN_TEST(test_euro_bit_change_without_owned_definitions_accepts_detector_factory_reset);
    RUN_TEST(test_euro_bit_change_restores_explicit_custom_definitions_after_user_write);
    RUN_TEST(test_usa_to_euro_from_advanced_requires_explicit_non_advanced_mode_before_writes);
    RUN_TEST(test_enabling_custom_filtering_without_profile_owned_ranges_is_rejected);
    RUN_TEST(test_enabling_custom_filtering_syncs_profile_owned_compact_ranges);
    RUN_TEST(test_enabling_custom_recommits_an_exact_complete_table_before_enable);
    RUN_TEST(test_region_change_with_custom_on_uses_verified_disabled_intermediate_then_table_then_final_enable);
    RUN_TEST(test_region_change_k_only_uses_fresh_target_ka_not_source_region_ka);
    RUN_TEST(test_region_partial_ownership_fails_closed_on_incomplete_target_refresh);
    RUN_TEST(test_region_refresh_request_failures_never_reach_table_or_final_enable);
    RUN_TEST(test_region_refresh_not_yet_retries_each_request_without_failing_apply);
    RUN_TEST(test_region_refresh_not_yet_has_bounded_send_deadline);
    RUN_TEST(test_custom_readback_request_not_yet_retries_while_failed_is_terminal);
    RUN_TEST(test_region_refresh_poisoned_definition_fails_before_table_or_final_enable);
    RUN_TEST(test_region_custom_on_disconnects_never_report_success_at_any_transaction_phase);
    RUN_TEST(test_k_only_authorship_preserves_live_ka_when_compiled_table_is_unchanged);
    RUN_TEST(test_k_only_authorship_replaces_k_and_copies_live_ka_without_invention);
    RUN_TEST(test_ka_only_authorship_relocates_around_and_preserves_live_k);
    RUN_TEST(test_unowned_band_readback_must_equal_the_fresh_live_definition);
    RUN_TEST(test_no_authored_ranges_infers_no_owned_bands_and_preserves_live_table);
    RUN_TEST(test_enabling_band_while_custom_is_on_requires_profile_owned_ranges);
    RUN_TEST(test_disabling_custom_filtering_does_not_require_or_write_definitions);
    RUN_TEST(test_unrelated_display_apply_does_not_require_sweeps_when_custom_was_already_enabled);
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
    RUN_TEST(test_queue_failures_preserve_durable_admission_taxonomy);
    return UNITY_END();
}
