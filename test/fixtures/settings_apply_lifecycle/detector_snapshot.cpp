#include <unity.h>
#include <new>
#include <string>
#include <vector>
#include "src/v1_devices.h"
#include "src/settings_types.h"
#include "src/modules/settings_operation/settings_operation_coordinator.h"

SerialClass Serial;
unsigned long mockMillis = 123456;
static std::vector<std::string> events;
static bool normalizeAvailable = true;

// Narrow collaborators: the production capture and entire connection callback
// are included below. Observation and snapshot payloads use production types.
struct NimBLEAddress {
    std::string value = "aa:bb:cc:dd:ee:ff";
    bool allocationFails = false;
    bool isNull() const { return value.empty(); }
    std::string toString() const {
        if (allocationFails) throw std::bad_alloc();
        return value;
    }
};
String normalizeV1DeviceAddress(const String& address) {
    if (!normalizeAvailable) return {};
    String result = address;
    result.toUpperCase();
    return result;
}

class DriveRuntime {
  public:
    static DriveRuntime* callbackOwner_;
    static void onV1Connected();
    void processStableDetector(bool requireLiveAddress);
    void serviceDeferredConnectionAutoPush();
    uint32_t pendingConnectionAutoPushSession_ = 0;
    uint32_t bootId_ = 17;
    struct AdmissionProbe {
        bool ownsConnection = false;
        bool allowsOrdinaryAutoPush() const { return !ownsConnection; }
        SettingsOperationCoordinator::StableAdmission onStableConnection() {
            events.push_back("operation-admission");
            return ownsConnection ? SettingsOperationCoordinator::StableAdmission::OwnedOrBlocked
                                  : SettingsOperationCoordinator::StableAdmission::AllowOrdinaryAutoPush;
        }
    } settingsOperationCoordinator_;
    bool connectedV1Address(String& address) {
        if (ble_.address.isNull()) return false;
        address = "AA:BB:CC:DD:EE:FF";
        return true;
    }
    struct Ble {
        NimBLEAddress address;
        uint32_t generation = 29, firmware = 41100, userRevision = 1, userIngress = 101;
        bool timedOut = false, copySucceeds = true, volume = true, sections = true,
             maximum = true, definitions = true;
        std::array<uint8_t, 6> userBytes{{1, 2, 3, 4, 5, 6}};
        std::vector<uint32_t> definitionBoundaries{100};
        mutable size_t boundaryReads = 0, copyCalls = 0;
        NimBLEAddress getConnectedAddress() const { return address; }
        bool connected = true;
        bool isConnected() const { return connected; }
        uint32_t sessionGeneration() const { return generation; }
        bool settingsCaptureTimedOut() const { events.push_back("capture"); return timedOut; }
        uint32_t v1FirmwareVersion() const { return firmware; }
        uint32_t sessionUserBytesRevision() const { return userRevision; }
        uint32_t sessionUserBytesIngressSequence() const { return userIngress; }
        bool copySessionUserBytes(uint8_t* out) const {
            ++copyCalls;
            if (!copySucceeds) return false;
            std::copy(userBytes.begin(), userBytes.end(), out);
            return true;
        }
        bool hasSessionAllVolume() const { return volume; }
        bool hasSessionSweepSectionsCapture() const { return sections; }
        bool hasSessionSweepMaxCapture() const { return maximum; }
        bool hasSessionSweepDefinitionsCapture() const { return definitions; }
        uint32_t sessionSweepDefinitionsIngressBoundary() const {
            const size_t index = std::min(boundaryReads++, definitionBoundaries.size() - 1);
            return definitionBoundaries[index];
        }
    } ble_;
    struct Parser {
        V1DisplayOnObservation display;
        V1BluetoothIndicatorObservation bluetooth;
        V1ModeObservation mode;
        V1AllVolumeObservation volume;
        V1SweepSectionsObservation sections;
        V1SweepMaxObservation maximum;
        V1SweepDefinitionsObservation definitions;
        const auto& displayOnObservation() const { return display; }
        const auto& bluetoothIndicatorObservation() const { return bluetooth; }
        const auto& modeObservation() const { return mode; }
        const auto& allVolumeObservation() const { return volume; }
        const auto& sweepSectionsObservation() const { return sections; }
        const auto& sweepMaxObservation() const { return maximum; }
        const auto& sweepDefinitionsObservation() const { return definitions; }
    } parser_;
    struct Catalog {
        V1DeviceDefaultProfileResult lookup{V1DeviceDefaultProfileStatus::NoOverride, 0};
        bool touch = true, ready = true, record = true;
        String profileAddress, recordedAddress;
        V1DetectorSnapshot saved;
        bool getInTheBoxForAddressChecked(const String&, V1InTheBoxSettings&) {
            events.push_back("app-settings"); return true;
        }
        auto getDeviceDefaultProfileChecked(const String& address) {
            events.push_back("lookup"); profileAddress = address; return lookup;
        }
        bool touchDeviceInMemory(const String&) { events.push_back("touch"); return touch; }
        bool isReady() const { return ready; }
        bool recordSnapshotInMemory(const String& address, const V1DetectorSnapshot& snapshot) {
            events.push_back("record"); recordedAddress = address; saved = snapshot; return record;
        }
    } devices_;
    struct Settings {
        V1Settings value;
        AutoPushSlot slot;
        String lastAddress;
        int requestedSlot = -1;
        bool pendingRecovery = false;
        bool recoveryPending() const { return pendingRecovery; }
        const V1Settings& get() const { return value; }
        void setLastV1Address(const String& address) { events.push_back("last-address"); lastAddress = address; }
        void requestLastV1AddressFallbackPersist(const String&) { events.push_back("fallback-persist"); }
        const AutoPushSlot& getSlot(int selected) {
            events.push_back("slot"); requestedSlot = selected; return slot;
        }
    } settings_;
    struct Quiet {
        uint32_t generation = 0;
        void setInTheBoxSettings(const V1InTheBoxSettings&, uint32_t session) {
            events.push_back("quiet"); generation = session;
        }
    } quiet_;
    struct AutoPush {
        V1DetectorSnapshot before;
        int queuedSlot = -1;
        bool activate = true, immediate = true;
        void setPreApplySnapshot(const V1DetectorSnapshot& snapshot) {
            events.push_back("preapply"); before = snapshot;
        }
        bool queueSlotPush(int slot, bool activateSlot, bool now) {
            events.push_back("queue"); queuedSlot = slot; activate = activateSlot; immediate = now;
            return true;
        }
    } autoPush_;
    struct Display {
        int selected = -1;
        void setProfileIndicatorSlot(int slot, uint32_t) { events.push_back("display"); selected = slot; }
    } display_;
#include "snapshot_declarations.inc"
    V1DetectorSnapshot operationSnapshot(uint32_t boundary) const {
        return captureDetectorSnapshot(SnapshotPurpose::SettingsOperation, boundary);
    }
};
DriveRuntime* DriveRuntime::callbackOwner_ = nullptr;
#include "detector_snapshot.inc"

void setUp() { events.clear(); normalizeAvailable = true; DriveRuntime::callbackOwner_ = nullptr; }
void tearDown() {}

static DriveRuntime ready() {
    DriveRuntime runtime;
    runtime.settings_.value.autoPushEnabled = true;
    runtime.settings_.value.activeSlot = 1;
    runtime.settings_.value.lastV1Address = "";
    auto& p = runtime.parser_;
    p.display = {7, 8, 101, true, false};
    p.bluetooth = {7, 8, 101, true, true, false, V1BluetoothIndicatorState::Blinking};
    p.mode = {7, 8, 101, true, 'U'};
    p.volume = {7, 8, 101, true, 2, 3, 4, 5};
    p.sections.available = p.sections.complete = true;
    p.sections.ingressSequence = 101;
    p.sections.count = 2;
    for (size_t i = 0; i < p.sections.sections.size(); ++i)
        p.sections.sections[i] = {static_cast<uint8_t>(i), 2,
                                 static_cast<uint16_t>(24000 + i), static_cast<uint16_t>(24100 + i)};
    p.maximum.available = true;
    p.maximum.ingressSequence = 101;
    p.maximum.maxIndex = 1;
    p.definitions.ingressSequence = 101;
    p.definitions.presentMask = 3;
    p.definitions.ingressSequences.fill(101);
    for (size_t i = 0; i < p.definitions.definitions.size(); ++i)
        p.definitions.definitions[i] = {static_cast<uint8_t>(i),
                                       static_cast<uint16_t>(33000 + i), static_cast<uint16_t>(33100 + i)};
    return runtime;
}

static V1DetectorSnapshot complete(const DriveRuntime& r) {
    V1DetectorSnapshot expected;
    expected.available = true;
    expected.capturedBootId = 17;
    expected.capturedUptimeMs = 123456;
    expected.sessionGeneration = 29;
    expected.hasFirmwareVersion = true;
    expected.firmwareVersion = 41100;
    expected.hasUserBytes = true;
    expected.userBytes = {{1, 2, 3, 4, 5, 6}};
    expected.hasMode = true; expected.mode = 'U';
    expected.hasDisplayOn = true; expected.displayOn = false;
    expected.hasBluetoothIndicator = true; expected.bluetoothIndicator = V1BluetoothIndicatorState::Blinking;
    expected.hasCurrentVolume = expected.hasSavedVolume = true;
    expected.currentMainVolume = 2; expected.currentMutedVolume = 3;
    expected.savedMainVolume = 4; expected.savedMutedVolume = 5;
    expected.hasSweepSections = true; expected.sweepSectionCount = 2;
    expected.sweepSections = r.parser_.sections.sections;
    expected.hasMaxSweepIndex = true; expected.maxSweepIndex = 1;
    expected.hasSweepDefinitions = true; expected.sweepDefinitions = r.parser_.definitions.definitions;
    return expected;
}

// Compare named values rather than struct padding, including unavailable scalar
// payloads and all array tails. Every case below uses this complete comparison.
static void equal(const V1DetectorSnapshot& expected, const V1DetectorSnapshot& actual) {
#define FIELD(name) TEST_ASSERT_EQUAL_MESSAGE(expected.name, actual.name, #name)
    FIELD(available); FIELD(capturedBootId); FIELD(capturedUptimeMs); FIELD(sessionGeneration);
    FIELD(captureTimedOut); FIELD(hasFirmwareVersion); FIELD(firmwareVersion); FIELD(hasUserBytes);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.userBytes.data(), actual.userBytes.data(), 6);
    FIELD(hasMode); FIELD(mode); FIELD(hasDisplayOn); FIELD(displayOn);
    FIELD(hasBluetoothIndicator); FIELD(bluetoothIndicator);
    FIELD(hasCurrentVolume); FIELD(currentMainVolume); FIELD(currentMutedVolume);
    FIELD(hasSavedVolume); FIELD(savedMainVolume); FIELD(savedMutedVolume);
    FIELD(hasSweepSections); FIELD(sweepSectionCount); FIELD(hasMaxSweepIndex); FIELD(maxSweepIndex);
    FIELD(hasSweepDefinitions);
    for (size_t i = 0; i < expected.sweepSections.size(); ++i) {
        FIELD(sweepSections[i].index); FIELD(sweepSections[i].count);
        FIELD(sweepSections[i].lowerMHz); FIELD(sweepSections[i].upperMHz);
    }
    for (size_t i = 0; i < expected.sweepDefinitions.size(); ++i) {
        FIELD(sweepDefinitions[i].index); FIELD(sweepDefinitions[i].lowerMHz); FIELD(sweepDefinitions[i].upperMHz);
    }
#undef FIELD
}
static V1DetectorSnapshot connectionSnapshot(DriveRuntime& r) {
    DriveRuntime::callbackOwner_ = &r;
    DriveRuntime::onV1Connected();
    return r.autoPush_.before;
}
static void noDefinitions(V1DetectorSnapshot& snapshot) {
    snapshot.hasSweepDefinitions = false; snapshot.sweepDefinitions = {};
}
static void eventsAre(std::initializer_list<const char*> expected) {
    TEST_ASSERT_EQUAL_UINT(expected.size(), events.size());
    size_t index = 0;
    for (const char* event : expected) TEST_ASSERT_EQUAL_STRING(event, events[index++].c_str());
}

static void test_complete_mapping_and_connection_handoff() {
    auto r = ready();
    const auto expected = complete(r);
    equal(expected, r.operationSnapshot(100));
    TEST_ASSERT_EQUAL_UINT(0, r.ble_.boundaryReads);
    events.clear();
    equal(expected, connectionSnapshot(r));
    equal(expected, r.devices_.saved);
    TEST_ASSERT_EQUAL_UINT(2, r.ble_.boundaryReads);
    TEST_ASSERT_EQUAL_UINT(29, r.quiet_.generation);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", r.devices_.recordedAddress.c_str());
    TEST_ASSERT_EQUAL_INT(1, r.autoPush_.queuedSlot);
    TEST_ASSERT_FALSE(r.autoPush_.activate); TEST_ASSERT_FALSE(r.autoPush_.immediate);
    eventsAre({"app-settings", "quiet", "operation-admission", "lookup", "touch", "last-address",
               "capture", "preapply", "record", "slot", "display", "queue"});
}

static void test_unavailable_scalars_keep_payloads_and_arrays_keep_defaults() {
    for (int condition = 0; condition < 4; ++condition) {
        auto r = ready(); auto expected = complete(r);
        auto& p = r.parser_;
        if (condition == 0) {
            p.display.available = p.bluetooth.available = p.mode.available = p.volume.available = false;
            p.sections.available = p.maximum.available = false;
            r.ble_.userRevision = 0;
        } else if (condition == 1) {
            p.display.revision = p.bluetooth.revision = p.mode.revision = p.volume.revision = 0;
            r.ble_.sections = r.ble_.maximum = false;
            r.ble_.userIngress = 0;
        } else if (condition == 2) {
            p.display.ingressSequence = p.bluetooth.ingressSequence = p.mode.ingressSequence = 0;
            p.volume.ingressSequence = p.sections.ingressSequence = p.maximum.ingressSequence = 0;
            r.ble_.copySucceeds = false;
        } else {
            p.display.ingressSequence = p.bluetooth.ingressSequence = p.mode.ingressSequence = 100;
            r.ble_.volume = false; p.sections.complete = false; p.maximum.poisoned = true;
            r.ble_.userIngress = 100;
        }
        expected.hasUserBytes = false; expected.userBytes.fill(0xFF);
        expected.hasDisplayOn = expected.hasBluetoothIndicator = expected.hasMode = false;
        expected.hasCurrentVolume = expected.hasSavedVolume = false;
        expected.hasSweepSections = false; expected.sweepSectionCount = 0; expected.sweepSections = {};
        expected.hasMaxSweepIndex = false; noDefinitions(expected);
        equal(expected, r.operationSnapshot(100));
        TEST_ASSERT_EQUAL_UINT(condition == 2 ? 1 : 0, r.ble_.copyCalls);
        TEST_ASSERT_EQUAL_UINT(0, r.ble_.boundaryReads);
        if (condition < 3) equal(expected, connectionSnapshot(r));
    }
}

static void test_capture_provenance_and_partial_observations() {
    auto r = ready(); auto expected = complete(r);
    r.bootId_ = expected.capturedBootId = 77;
    r.ble_.generation = expected.sessionGeneration = 99;
    r.ble_.firmware = expected.firmwareVersion = 0;
    expected.hasFirmwareVersion = false;
    r.ble_.timedOut = expected.captureTimedOut = true;
    // One unavailable observation must not suppress unrelated captured values.
    r.parser_.mode.available = expected.hasMode = false;
    equal(expected, r.operationSnapshot(0)); equal(expected, connectionSnapshot(r));
}

static void test_definition_rows_use_purpose_specific_boundaries() {
    struct Case { uint32_t ingress, operationBoundary, requestBoundary; bool operation, connection; };
    const Case cases[] = {
        {101, 0, 0, true, false}, {101, 0, 101, true, false},
        {101, 0, 102, true, false}, {101, 0, 100, true, true},
        {101, 100, 102, true, false}, {101, 102, 100, false, true},
        {0, 0, 100, false, false}, {100, 100, 100, false, false},
        {99, 100, 100, false, false}, {1, UINT32_MAX, UINT32_MAX, true, true},
        {UINT32_MAX, 1, 1, false, false}, {0x80000001u, 1, 1, false, false},
    };
    for (const auto& c : cases) {
        auto r = ready(); auto expected = complete(r);
        // Aggregate and ordinary fields stay admitted while only one row varies.
        r.parser_.definitions.ingressSequence = c.operationBoundary == UINT32_MAX ? 1 : c.operationBoundary + 1;
        r.parser_.definitions.ingressSequences[0] = c.ingress;
        r.parser_.definitions.ingressSequences[1] = c.ingress;
        r.ble_.definitionBoundaries = {c.requestBoundary};
        // Keep all non-definition observation gates outside this boundary table.
        r.ble_.userIngress = r.parser_.display.ingressSequence = r.parser_.bluetooth.ingressSequence =
            r.parser_.mode.ingressSequence = r.parser_.volume.ingressSequence =
            r.parser_.sections.ingressSequence = r.parser_.maximum.ingressSequence =
                r.parser_.definitions.ingressSequence;
        if (!c.operation) noDefinitions(expected);
        equal(expected, r.operationSnapshot(c.operationBoundary));
        TEST_ASSERT_EQUAL_UINT(0, r.ble_.boundaryReads);
        expected = complete(r); if (!c.connection) noDefinitions(expected);
        equal(expected, connectionSnapshot(r));
        TEST_ASSERT_EQUAL_UINT(c.connection ? 2 : 1, r.ble_.boundaryReads);
    }
}

static void test_aggregate_and_individual_freshness_remain_separate() {
    for (int condition = 0; condition < 3; ++condition) {
        auto r = ready(); auto expected = complete(r);
        if (condition == 0) r.parser_.definitions.ingressSequence = 99;
        if (condition == 1) r.parser_.definitions.ingressSequences[1] = 99;
        if (condition == 2) r.parser_.definitions.ingressSequence = 0;
        noDefinitions(expected);
        equal(expected, r.operationSnapshot(100));
        if (condition == 0) expected = complete(r);
        equal(expected, connectionSnapshot(r));
        TEST_ASSERT_EQUAL_UINT(condition == 2 ? 0 : 2, r.ble_.boundaryReads);
    }
}

static void test_sweep_eligibility_masks_and_payload_tails() {
    for (int condition = 0; condition < 8; ++condition) {
        auto r = ready(); auto expected = complete(r);
        switch (condition) {
        case 0: r.parser_.definitions.poisoned = true; break;
        case 1: r.parser_.definitions.presentMask = 1; break;
        case 2: r.parser_.definitions.presentMask = 7; break;
        case 3: r.ble_.definitions = false; break;
        case 4: r.parser_.maximum.maxIndex = expected.maxSweepIndex = 0;
                r.parser_.definitions.presentMask = 1; break;
        case 5: r.parser_.maximum.maxIndex = expected.maxSweepIndex = 63;
                r.parser_.definitions.presentMask = UINT64_MAX; break;
        case 6: r.parser_.sections.poisoned = true; break; // Existing complete observation policy.
        case 7: r.parser_.sections.revision = r.parser_.maximum.revision = 0; break;
        }
        if (condition < 4) noDefinitions(expected);
        equal(expected, r.operationSnapshot(0)); equal(expected, connectionSnapshot(r));
        TEST_ASSERT_EQUAL_UINT(condition < 4 ? 0 : expected.maxSweepIndex + 1, r.ble_.boundaryReads);
    }
}

static void test_request_boundary_is_read_at_each_admitted_row() {
    auto r = ready(); auto expected = complete(r);
    r.ble_.definitionBoundaries = {100, 101};
    noDefinitions(expected);
    equal(expected, connectionSnapshot(r));
    TEST_ASSERT_EQUAL_UINT(2, r.ble_.boundaryReads);
    r = ready(); expected = complete(r);
    r.parser_.definitions.ingressSequences[0] = 0;
    noDefinitions(expected);
    equal(expected, connectionSnapshot(r));
    TEST_ASSERT_EQUAL_UINT(1, r.ble_.boundaryReads); // Even a zero row obtains its boundary before rejection.
}

static void test_connection_early_exits_do_not_capture_or_queue() {
    DriveRuntime::onV1Connected(); eventsAre({});
    for (int condition = 0; condition < 7; ++condition) {
        events.clear(); auto r = ready();
        if (condition == 0) r.settingsOperationCoordinator_.ownsConnection = true;
        if (condition == 1) r.ble_.address.value = "short";
        if (condition == 2) r.ble_.address.allocationFails = true;
        if (condition == 3) normalizeAvailable = false;
        if (condition == 4) r.devices_.lookup.status = V1DeviceDefaultProfileStatus::Unavailable;
        if (condition == 5) r.devices_.touch = false;
        if (condition == 6) { r.ble_.address.value.clear(); r.settings_.value.lastV1Address = "short"; }
        connectionSnapshot(r);
        TEST_ASSERT_FALSE(r.autoPush_.before.available);
        TEST_ASSERT_EQUAL_INT(-1, r.autoPush_.queuedSlot);
        TEST_ASSERT_EQUAL_UINT(0, r.ble_.copyCalls);
        TEST_ASSERT_EQUAL_UINT(0, r.ble_.boundaryReads);
        normalizeAvailable = true;
    }
}

static void test_connection_store_failure_stops_after_preapply_handoff() {
    auto r = ready(); r.devices_.record = false;
    equal(complete(r), connectionSnapshot(r));
    eventsAre({"app-settings", "quiet", "operation-admission", "lookup", "touch", "last-address",
               "capture", "preapply", "record"});
    TEST_ASSERT_EQUAL_INT(-1, r.autoPush_.queuedSlot);
}

static void test_connection_slot_selection_and_disabled_autopush() {
    for (int slot : {-1, 0, 1, 2, 9}) {
        for (uint8_t overrideProfile = 0; overrideProfile <= 3; ++overrideProfile) {
            auto r = ready(); r.settings_.value.activeSlot = slot;
            if (overrideProfile) r.devices_.lookup = {V1DeviceDefaultProfileStatus::Found, overrideProfile};
            connectionSnapshot(r);
            const int expected = overrideProfile ? overrideProfile - 1 : std::max(0, std::min(2, slot));
            TEST_ASSERT_EQUAL_INT(expected, r.autoPush_.queuedSlot);
            TEST_ASSERT_EQUAL_INT(expected, r.display_.selected);
            TEST_ASSERT_EQUAL_INT(expected, r.settings_.requestedSlot);
        }
    }
    events.clear(); auto r = ready(); r.settings_.value.autoPushEnabled = false;
    equal(complete(r), connectionSnapshot(r)); equal(complete(r), r.devices_.saved);
    TEST_ASSERT_EQUAL_INT(-1, r.autoPush_.queuedSlot);
    eventsAre({"app-settings", "quiet", "operation-admission", "lookup", "touch", "last-address",
               "capture", "preapply", "record", "slot"});
}

static void test_fallback_and_absent_identity_paths() {
    auto r = ready(); r.ble_.address.value.clear(); r.settings_.value.lastV1Address = "aa:bb:cc:dd:ee:ff";
    equal(complete(r), connectionSnapshot(r));
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", r.devices_.profileAddress.c_str());
    eventsAre({"quiet", "operation-admission", "lookup", "touch", "capture", "preapply", "slot", "display", "queue"});
    events.clear(); r = ready(); r.devices_.ready = false;
    equal(complete(r), connectionSnapshot(r));
    eventsAre({"app-settings", "quiet", "operation-admission", "lookup", "touch", "last-address",
               "fallback-persist", "capture", "preapply", "slot", "display", "queue"});
    events.clear(); r = ready(); r.ble_.address.value.clear();
    connectionSnapshot(r); eventsAre({"quiet", "operation-admission"});
    TEST_ASSERT_FALSE(r.autoPush_.before.available);
    events.clear(); r.settings_.value.autoPushEnabled = false;
    equal(complete(r), connectionSnapshot(r));
    eventsAre({"quiet", "operation-admission", "capture", "preapply", "slot"});
}

static void test_deferred_autopush_uses_recovered_slot_only_for_the_live_session() {
    for (int condition = 0; condition < 5; ++condition) {
        events.clear();
        auto r = ready();
        r.settings_.pendingRecovery = true;
        equal(complete(r), connectionSnapshot(r));
        TEST_ASSERT_EQUAL_INT(-1, r.autoPush_.queuedSlot);
        TEST_ASSERT_EQUAL_INT(-1, r.display_.selected);
        TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", r.devices_.recordedAddress.c_str());
        r.serviceDeferredConnectionAutoPush();
        TEST_ASSERT_EQUAL_UINT32(r.ble_.generation, r.pendingConnectionAutoPushSession_);
        TEST_ASSERT_EQUAL_INT(-1, r.autoPush_.queuedSlot);

        r.settings_.pendingRecovery = false;
        r.settings_.value.activeSlot = 2;
        if (condition == 1) ++r.ble_.generation;
        if (condition == 2) r.ble_.connected = false;
        if (condition == 3) r.settingsOperationCoordinator_.ownsConnection = true;
        if (condition == 4) {
            r.ble_.address.value.clear();
            r.settings_.value.lastV1Address = "AA:BB:CC:DD:EE:FF";
        }
        events.clear();
        r.serviceDeferredConnectionAutoPush();
        TEST_ASSERT_EQUAL_UINT32(0, r.pendingConnectionAutoPushSession_);
        TEST_ASSERT_EQUAL_INT(condition == 0 ? 2 : -1, r.autoPush_.queuedSlot);
        TEST_ASSERT_EQUAL_INT(condition == 0 ? 2 : -1, r.display_.selected);
        if (condition != 0) eventsAre({});
        events.clear();
        r.serviceDeferredConnectionAutoPush();
        eventsAre({});
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_complete_mapping_and_connection_handoff);
    RUN_TEST(test_unavailable_scalars_keep_payloads_and_arrays_keep_defaults);
    RUN_TEST(test_capture_provenance_and_partial_observations);
    RUN_TEST(test_definition_rows_use_purpose_specific_boundaries);
    RUN_TEST(test_aggregate_and_individual_freshness_remain_separate);
    RUN_TEST(test_sweep_eligibility_masks_and_payload_tails);
    RUN_TEST(test_request_boundary_is_read_at_each_admitted_row);
    RUN_TEST(test_connection_early_exits_do_not_capture_or_queue);
    RUN_TEST(test_connection_store_failure_stops_after_preapply_handoff);
    RUN_TEST(test_connection_slot_selection_and_disabled_autopush);
    RUN_TEST(test_fallback_and_absent_identity_paths);
    RUN_TEST(test_deferred_autopush_uses_recovered_slot_only_for_the_live_session);
    return UNITY_END();
}
