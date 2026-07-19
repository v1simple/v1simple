#include <unity.h>

#include <initializer_list>
#include <vector>

#include "../../src/modules/wifi/wifi_scan_result_owner.h"
#ifndef V1_LINKED_TEST_WIFI_SCAN_RESULT_OWNER
#include "../../src/modules/wifi/wifi_scan_result_owner.cpp"
#endif

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {

constexpr int16_t SCAN_RUNNING = -1;
constexpr int16_t SCAN_FAILED = -2;

struct RawNetwork {
    String ssid;
    int32_t rssi;
    uint8_t encryptionType;
};

struct FakeScanDriver {
    std::vector<RawNetwork> rawNetworks;
    int16_t status = SCAN_RUNNING;
    int16_t startResult = SCAN_RUNNING;
    int startCalls = 0;
    int releaseCalls = 0;
    int abortCalls = 0;
    int readsAfterRelease = 0;
    bool released = false;
    bool hardwareActive = false;

    void complete(std::initializer_list<RawNetwork> networks) {
        rawNetworks.assign(networks.begin(), networks.end());
        status = static_cast<int16_t>(rawNetworks.size());
        released = false;
        hardwareActive = false;
    }

    void fail() {
        status = SCAN_FAILED;
        released = false;
    }
};

int16_t startScan(void* ctx) {
    auto* fake = static_cast<FakeScanDriver*>(ctx);
    fake->startCalls++;
    if (fake->hardwareActive) {
        return SCAN_FAILED;
    }
    fake->rawNetworks.clear();
    fake->status = SCAN_RUNNING;
    fake->released = false;
    fake->hardwareActive = fake->startResult == SCAN_RUNNING;
    return fake->startResult;
}

int16_t scanStatus(void* ctx) {
    return static_cast<FakeScanDriver*>(ctx)->status;
}

const RawNetwork* rawNetworkAt(int16_t index, FakeScanDriver& fake) {
    if (fake.released) {
        fake.readsAfterRelease++;
        return nullptr;
    }
    if (index < 0 || static_cast<size_t>(index) >= fake.rawNetworks.size()) {
        return nullptr;
    }
    return &fake.rawNetworks[static_cast<size_t>(index)];
}

String scanSsidAt(int16_t index, void* ctx) {
    auto& fake = *static_cast<FakeScanDriver*>(ctx);
    const RawNetwork* network = rawNetworkAt(index, fake);
    return network ? network->ssid : String();
}

int32_t scanRssiAt(int16_t index, void* ctx) {
    auto& fake = *static_cast<FakeScanDriver*>(ctx);
    const RawNetwork* network = rawNetworkAt(index, fake);
    return network ? network->rssi : 0;
}

uint8_t scanEncryptionAt(int16_t index, void* ctx) {
    auto& fake = *static_cast<FakeScanDriver*>(ctx);
    const RawNetwork* network = rawNetworkAt(index, fake);
    return network ? network->encryptionType : WIFI_AUTH_OPEN;
}

void releaseScan(void* ctx) {
    auto* fake = static_cast<FakeScanDriver*>(ctx);
    fake->releaseCalls++;
    fake->released = true;
    fake->rawNetworks.clear();
}

void abortScan(void* ctx) {
    auto* fake = static_cast<FakeScanDriver*>(ctx);
    fake->abortCalls++;
    fake->released = true;
    fake->status = SCAN_FAILED;
    fake->hardwareActive = false;
    fake->rawNetworks.clear();
}

WifiScanResultOwner::Driver makeDriver(FakeScanDriver& fake) {
    WifiScanResultOwner::Driver driver;
    driver.ctx = &fake;
    driver.runningStatus = SCAN_RUNNING;
    driver.start = startScan;
    driver.status = scanStatus;
    driver.ssidAt = scanSsidAt;
    driver.rssiAt = scanRssiAt;
    driver.encryptionAt = scanEncryptionAt;
    driver.release = releaseScan;
    driver.abort = abortScan;
    return driver;
}

void assertNetwork(const ScannedNetwork& network, const char* ssid, int32_t rssi, uint8_t encryptionType) {
    TEST_ASSERT_EQUAL_STRING(ssid, network.ssid.c_str());
    TEST_ASSERT_EQUAL_INT32(rssi, network.rssi);
    TEST_ASSERT_EQUAL_UINT8(encryptionType, network.encryptionType);
}

} // namespace

void setUp() {}

void tearDown() {}

void test_maintenance_scan_can_be_joined_by_ui_and_harvested_once() {
    WifiScanResultOwner owner;
    FakeScanDriver fake;
    const auto driver = makeDriver(fake);

    TEST_ASSERT_EQUAL(WifiScanResultOwner::RequestResult::STARTED,
                      owner.request(WifiScanConsumer::MAINTENANCE, driver));
    TEST_ASSERT_EQUAL(WifiScanResultOwner::RequestResult::JOINED, owner.request(WifiScanConsumer::UI, driver));
    TEST_ASSERT_EQUAL_INT(1, fake.startCalls);

    fake.complete({{"Weak", -80, WIFI_AUTH_WPA2_PSK},
                   {"Strong", -35, WIFI_AUTH_OPEN},
                   {"Weak", -55, WIFI_AUTH_WPA3_PSK},
                   {"", -20, WIFI_AUTH_OPEN}});
    TEST_ASSERT_EQUAL(WifiScanResultOwner::HarvestResult::COMPLETED, owner.harvest(driver));

    TEST_ASSERT_EQUAL_INT(1, fake.releaseCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.readsAfterRelease);
    TEST_ASSERT_TRUE(owner.hasSnapshot(WifiScanConsumer::UI));
    TEST_ASSERT_TRUE(owner.hasSnapshot(WifiScanConsumer::MAINTENANCE));

    const auto uiNetworks = owner.copySnapshot(WifiScanConsumer::UI);
    const auto maintenanceNetworks = owner.copySnapshot(WifiScanConsumer::MAINTENANCE);
    TEST_ASSERT_EQUAL_UINT(2, uiNetworks.size());
    TEST_ASSERT_EQUAL_UINT(2, maintenanceNetworks.size());
    assertNetwork(uiNetworks[0], "Strong", -35, WIFI_AUTH_OPEN);
    assertNetwork(uiNetworks[1], "Weak", -55, WIFI_AUTH_WPA3_PSK);

    owner.clearSnapshot(WifiScanConsumer::MAINTENANCE);
    TEST_ASSERT_FALSE(owner.hasSnapshot(WifiScanConsumer::MAINTENANCE));
    TEST_ASSERT_TRUE(owner.hasSnapshot(WifiScanConsumer::UI));
    const auto repeatedUiNetworks = owner.copySnapshot(WifiScanConsumer::UI);
    TEST_ASSERT_EQUAL_UINT(2, repeatedUiNetworks.size());
    TEST_ASSERT_EQUAL_INT(1, fake.releaseCalls);
}

void test_next_ui_generation_preserves_unread_maintenance_snapshot() {
    WifiScanResultOwner owner;
    FakeScanDriver fake;
    const auto driver = makeDriver(fake);

    owner.request(WifiScanConsumer::UI, driver);
    owner.request(WifiScanConsumer::MAINTENANCE, driver);
    const uint32_t firstGeneration = owner.generation();
    fake.complete({{"Old", -70, WIFI_AUTH_WPA2_PSK}});
    owner.harvest(driver);

    TEST_ASSERT_EQUAL(WifiScanResultOwner::RequestResult::STARTED, owner.request(WifiScanConsumer::UI, driver));
    TEST_ASSERT_FALSE(owner.hasSnapshot(WifiScanConsumer::UI));
    TEST_ASSERT_TRUE(owner.hasSnapshot(WifiScanConsumer::MAINTENANCE));
    TEST_ASSERT_EQUAL_UINT32(firstGeneration, owner.snapshotGeneration(WifiScanConsumer::MAINTENANCE));

    fake.complete({{"New", -40, WIFI_AUTH_OPEN}});
    owner.harvest(driver);

    const auto uiNetworks = owner.copySnapshot(WifiScanConsumer::UI);
    const auto maintenanceNetworks = owner.copySnapshot(WifiScanConsumer::MAINTENANCE);
    TEST_ASSERT_EQUAL_UINT(1, uiNetworks.size());
    TEST_ASSERT_EQUAL_UINT(1, maintenanceNetworks.size());
    TEST_ASSERT_EQUAL_STRING("New", uiNetworks[0].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Old", maintenanceNetworks[0].ssid.c_str());
    TEST_ASSERT_EQUAL_INT(2, fake.startCalls);
    TEST_ASSERT_EQUAL_INT(2, fake.releaseCalls);
}

void test_canceling_maintenance_preserves_joint_ui_scan() {
    WifiScanResultOwner owner;
    FakeScanDriver fake;
    const auto driver = makeDriver(fake);

    owner.request(WifiScanConsumer::UI, driver);
    owner.request(WifiScanConsumer::MAINTENANCE, driver);

    TEST_ASSERT_TRUE(owner.cancel(WifiScanConsumer::MAINTENANCE, driver));
    TEST_ASSERT_TRUE(owner.isRunning());
    TEST_ASSERT_TRUE(owner.isPending(WifiScanConsumer::UI));
    TEST_ASSERT_FALSE(owner.isPending(WifiScanConsumer::MAINTENANCE));
    TEST_ASSERT_EQUAL_INT(0, fake.releaseCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.abortCalls);

    fake.complete({{"UI-only", -45, WIFI_AUTH_OPEN}});
    owner.harvest(driver);

    TEST_ASSERT_TRUE(owner.hasSnapshot(WifiScanConsumer::UI));
    TEST_ASSERT_FALSE(owner.hasSnapshot(WifiScanConsumer::MAINTENANCE));
    TEST_ASSERT_EQUAL_INT(1, fake.releaseCalls);
}

void test_canceling_last_consumer_releases_active_scan() {
    WifiScanResultOwner owner;
    FakeScanDriver fake;
    const auto driver = makeDriver(fake);

    owner.request(WifiScanConsumer::MAINTENANCE, driver);

    TEST_ASSERT_TRUE(owner.cancel(WifiScanConsumer::MAINTENANCE, driver));
    TEST_ASSERT_FALSE(owner.isRunning());
    TEST_ASSERT_EQUAL_INT(0, fake.releaseCalls);
    TEST_ASSERT_EQUAL_INT(1, fake.abortCalls);
    TEST_ASSERT_EQUAL(WifiScanResultOwner::HarvestResult::IDLE, owner.harvest(driver));
    TEST_ASSERT_EQUAL(WifiScanResultOwner::RequestResult::STARTED, owner.request(WifiScanConsumer::UI, driver));
    TEST_ASSERT_EQUAL_INT(2, fake.startCalls);
}

void test_zero_results_are_valid_snapshots_for_both_consumers() {
    WifiScanResultOwner owner;
    FakeScanDriver fake;
    const auto driver = makeDriver(fake);

    owner.request(WifiScanConsumer::MAINTENANCE, driver);
    owner.request(WifiScanConsumer::UI, driver);
    fake.complete({});

    TEST_ASSERT_EQUAL(WifiScanResultOwner::HarvestResult::COMPLETED, owner.harvest(driver));
    TEST_ASSERT_TRUE(owner.hasSnapshot(WifiScanConsumer::UI));
    TEST_ASSERT_TRUE(owner.hasSnapshot(WifiScanConsumer::MAINTENANCE));
    TEST_ASSERT_TRUE(owner.copySnapshot(WifiScanConsumer::UI).empty());
    TEST_ASSERT_TRUE(owner.copySnapshot(WifiScanConsumer::MAINTENANCE).empty());
    TEST_ASSERT_EQUAL_INT(1, fake.releaseCalls);
}

void test_sta_drop_gate_waits_for_the_remaining_ui_consumer() {
    WifiScanResultOwner owner;
    WifiScanStaDropGate dropGate;
    FakeScanDriver fake;
    const auto driver = makeDriver(fake);

    owner.request(WifiScanConsumer::UI, driver);
    owner.request(WifiScanConsumer::MAINTENANCE, driver);
    owner.cancel(WifiScanConsumer::MAINTENANCE, driver);
    dropGate.request();

    TEST_ASSERT_TRUE(dropGate.pending());
    TEST_ASSERT_FALSE(dropGate.takeIfReady(owner.isRunning()));

    fake.complete({{"UI-owned", -42, WIFI_AUTH_OPEN}});
    owner.harvest(driver);

    TEST_ASSERT_TRUE(dropGate.takeIfReady(owner.isRunning()));
    TEST_ASSERT_FALSE(dropGate.pending());
    TEST_ASSERT_FALSE(dropGate.takeIfReady(owner.isRunning()));
}

void test_rejoining_maintenance_clears_a_deferred_sta_drop() {
    WifiScanResultOwner owner;
    WifiScanStaDropGate dropGate;
    FakeScanDriver fake;
    const auto driver = makeDriver(fake);

    owner.request(WifiScanConsumer::UI, driver);
    owner.request(WifiScanConsumer::MAINTENANCE, driver);
    owner.cancel(WifiScanConsumer::MAINTENANCE, driver);
    dropGate.request();

    dropGate.clear();
    TEST_ASSERT_EQUAL(WifiScanResultOwner::RequestResult::JOINED, owner.request(WifiScanConsumer::MAINTENANCE, driver));
    fake.complete({{"Rejoined", -41, WIFI_AUTH_OPEN}});
    owner.harvest(driver);

    TEST_ASSERT_FALSE(dropGate.pending());
    TEST_ASSERT_FALSE(dropGate.takeIfReady(owner.isRunning()));
    TEST_ASSERT_TRUE(owner.hasSnapshot(WifiScanConsumer::UI));
    TEST_ASSERT_TRUE(owner.hasSnapshot(WifiScanConsumer::MAINTENANCE));
}

void test_rejected_maintenance_restart_preserves_deferred_sta_drop() {
    WifiScanResultOwner owner;
    WifiScanStaDropGate dropGate;
    FakeScanDriver fake;
    const auto driver = makeDriver(fake);

    owner.request(WifiScanConsumer::UI, driver);
    owner.request(WifiScanConsumer::MAINTENANCE, driver);
    owner.cancel(WifiScanConsumer::MAINTENANCE, driver);
    dropGate.request();

    TEST_ASSERT_TRUE(dropGate.pending());
    fake.complete({{"UI-only", -43, WIFI_AUTH_OPEN}});
    owner.harvest(driver);

    TEST_ASSERT_TRUE(dropGate.takeIfReady(owner.isRunning()));
    TEST_ASSERT_FALSE(dropGate.pending());
}

void test_failed_scan_aborts_hardware_and_allows_fresh_request() {
    WifiScanResultOwner owner;
    FakeScanDriver fake;
    const auto driver = makeDriver(fake);

    owner.request(WifiScanConsumer::UI, driver);
    fake.fail();

    TEST_ASSERT_EQUAL(WifiScanResultOwner::HarvestResult::FAILED, owner.harvest(driver));
    TEST_ASSERT_FALSE(owner.isRunning());
    TEST_ASSERT_FALSE(owner.isPending(WifiScanConsumer::UI));
    TEST_ASSERT_FALSE(owner.hasSnapshot(WifiScanConsumer::UI));
    TEST_ASSERT_EQUAL_INT(0, fake.releaseCalls);
    TEST_ASSERT_EQUAL_INT(1, fake.abortCalls);

    TEST_ASSERT_EQUAL(WifiScanResultOwner::RequestResult::STARTED, owner.request(WifiScanConsumer::UI, driver));
    TEST_ASSERT_EQUAL_INT(2, fake.startCalls);
}

void test_start_failure_aborts_stale_hardware_before_retry() {
    WifiScanResultOwner owner;
    FakeScanDriver fake;
    const auto driver = makeDriver(fake);
    fake.hardwareActive = true;

    TEST_ASSERT_EQUAL(WifiScanResultOwner::RequestResult::FAILED, owner.request(WifiScanConsumer::UI, driver));
    TEST_ASSERT_EQUAL_INT(1, fake.startCalls);
    TEST_ASSERT_EQUAL_INT(1, fake.abortCalls);
    TEST_ASSERT_FALSE(fake.hardwareActive);

    TEST_ASSERT_EQUAL(WifiScanResultOwner::RequestResult::STARTED, owner.request(WifiScanConsumer::UI, driver));
    TEST_ASSERT_EQUAL_INT(2, fake.startCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_maintenance_scan_can_be_joined_by_ui_and_harvested_once);
    RUN_TEST(test_next_ui_generation_preserves_unread_maintenance_snapshot);
    RUN_TEST(test_canceling_maintenance_preserves_joint_ui_scan);
    RUN_TEST(test_canceling_last_consumer_releases_active_scan);
    RUN_TEST(test_zero_results_are_valid_snapshots_for_both_consumers);
    RUN_TEST(test_sta_drop_gate_waits_for_the_remaining_ui_consumer);
    RUN_TEST(test_rejoining_maintenance_clears_a_deferred_sta_drop);
    RUN_TEST(test_rejected_maintenance_restart_preserves_deferred_sta_drop);
    RUN_TEST(test_failed_scan_aborts_hardware_and_allows_fresh_request);
    RUN_TEST(test_start_failure_aborts_stale_hardware_before_retry);
    return UNITY_END();
}
