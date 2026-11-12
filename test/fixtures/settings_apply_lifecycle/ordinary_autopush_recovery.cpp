#define main original_settings_suite_main
#include "test/test_settings_persistence/test_settings_persistence.cpp"
#undef main
#include "src/v1_devices.h"
#include "src/modules/settings_operation/settings_operation_coordinator.h"

struct NimBLEAddress {
    bool isNull() const { return false; }
    std::string toString() const { return "aa:bb:cc:dd:ee:ff"; }
};

String normalizeV1DeviceAddress(const String& address) {
    String normalized = address;
    normalized.toUpperCase();
    return normalized;
}

class DriveRuntime {
  public:
    static DriveRuntime* callbackOwner_;
    static void onV1Connected();
    void processStableDetector(bool requireLiveAddress);
    void serviceDeferredConnectionAutoPush();
    uint32_t pendingConnectionAutoPushSession_ = 0;
    bool connectedV1Address(String& address) {
        address = "AA:BB:CC:DD:EE:FF";
        return true;
    }
    const uint32_t bootId_ = 17;
    SettingsManager& settings_;
    struct ConnectedTransport : V1BLEClient {
        NimBLEAddress getConnectedAddress() const { return {}; }
    } ble_;
    PacketParser parser_;
    QuietCoordinatorModule quiet_;
    AutoPushModule autoPush_;
    V1Display display_;
    struct Catalog {
        unsigned captures = 0;
        V1DetectorSnapshot latest;
        bool getInTheBoxForAddressChecked(const String&, V1InTheBoxSettings&) { return true; }
        V1DeviceDefaultProfileResult getDeviceDefaultProfileChecked(const String&) {
            return {V1DeviceDefaultProfileStatus::NoOverride, 0};
        }
        bool touchDeviceInMemory(const String&) { return true; }
        bool isReady() const { return true; }
        bool recordSnapshotInMemory(const String&, const V1DetectorSnapshot& snapshot) {
            latest = snapshot;
            ++captures;
            return true;
        }
    } devices_;
    struct EmptyOperationStore {
        bool allowsOrdinaryAutoPush() const { return true; }
        SettingsOperationCoordinator::StableAdmission onStableConnection() {
            return SettingsOperationCoordinator::StableAdmission::AllowOrdinaryAutoPush;
        }
    } settingsOperationCoordinator_;

    explicit DriveRuntime(SettingsManager& settings) : settings_(settings) {
        quiet_.begin(&ble_, &parser_);
        autoPush_.begin(&settings_, &profiles, &ble_, &parser_, [](int, void*) {}, this, &quiet_);
    }
    void seedDetector() {
        parseCanonicalSettingsPacket(parser_, ble_, PACKET_ID_RESP_VERSION,
                                     {'v', '4', '.', '1', '0', '3', '9'}, 0xD6);
        ble_.onV1FirmwareVersionReceived(41039);
        observeSettingsDisplay(parser_, ble_, false, 1);
    }
#include "snapshot_declarations.inc"
};

DriveRuntime* DriveRuntime::callbackOwner_ = nullptr;
#include "detector_snapshot.inc"

void test_unreadable_settings_cannot_admit_detector_display_write() {
    fs::FS filesystem(g_tempRoot);
    storage.setFilesystem(&filesystem, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager source(storage, profiles);
    makeCurrentV21Source(source.mutableSettings());
    source.mutableSettings().autoPushEnabled = false;
    TEST_ASSERT_TRUE(source.save());
    const String newest = activeNamespaceOrEmpty();
    mock_preferences::set_fail_begin_for_namespace(newest.c_str());
    SettingsManager boot(storage, profiles);
    boot.begin();
    TEST_ASSERT_TRUE(boot.recoveryPending());
    TEST_ASSERT_TRUE(boot.get().autoPushEnabled);
    TEST_ASSERT_EQUAL_UINT32(0, boot.get().autoPushProfileSchemaVersion);
    mockMillis = 3000;
    DriveRuntime runtime(boot);
    runtime.seedDetector();
    DriveRuntime::callbackOwner_ = &runtime;
    DriveRuntime::onV1Connected();
    TEST_ASSERT_EQUAL_UINT(1, runtime.devices_.captures);
    TEST_ASSERT_TRUE(runtime.devices_.latest.hasDisplayOn);
    TEST_ASSERT_EQUAL_UINT32(runtime.ble_.sessionGeneration(), runtime.pendingConnectionAutoPushSession_);
    for (mockMillis = 3100; mockMillis < 3300; mockMillis += 10) {
        runtime.autoPush_.process();
    }
    TEST_ASSERT_TRUE(boot.recoveryPending());
    mock_preferences::set_fail_begin_for_namespace(nullptr);
    boot.serviceDeferredPersist(3300);
    TEST_ASSERT_FALSE(boot.recoveryPending());
    runtime.serviceDeferredConnectionAutoPush();
    TEST_ASSERT_EQUAL_UINT32(0, runtime.pendingConnectionAutoPushSession_);
    for (mockMillis = 3300; mockMillis < 3500; mockMillis += 10) {
        runtime.autoPush_.process();
    }
    SettingsManager readback(storage, profiles);
    readback.load();
    TEST_ASSERT_FALSE(readback.get().autoPushEnabled);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, runtime.ble_.setDisplayOnCalls,
        "Connection AutoPush sent a display write although saved AutoPush was off and settings were unreadable");
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_unreadable_settings_cannot_admit_detector_display_write);
    return UNITY_END();
}
