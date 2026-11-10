// Real bond backup/recovery owners plus the extracted pinned NVS loader and
// complete V1BLEClient::initBLE method. Radio, NVS and task scheduling are modeled.
#define main existing_bond_writer_suite_main
#include "test/test_ble_bond_backup_writer/test_ble_bond_backup_writer.cpp"
#undef main
#include "src/ble_fresh_flash_policy.cpp"

namespace PinnedNimble {
#define MYNEWT_VAL(name) BOOTSTRAP_##name
#define BOOTSTRAP_BLE_STORE_MAX_BONDS 3
#define BOOTSTRAP_BLE_STORE_MAX_CCCDS 1
#define BOOTSTRAP_BLE_STORE_MAX_CSFCS 1
#define BOOTSTRAP_BLE_HOST_BASED_PRIVACY 1
#define BOOTSTRAP_ENC_ADV_DATA 1
#define ESP_LOGE(...) (++loggedErrors)
#define ESP_LOGD(...) ((void)0)
#define BLE_STORE_OBJ_TYPE_CCCD 3
#define BLE_STORE_OBJ_TYPE_CSFC 4
#define BLE_STORE_OBJ_TYPE_LOCAL_IRK 5
#define BLE_STORE_OBJ_TYPE_PEER_ADDR 6
#define BLE_STORE_OBJ_TYPE_PEER_DEV_REC 7
#define BLE_STORE_OBJ_TYPE_ENC_ADV_DATA 8
#define NIMBLE_NVS_STR_NAME_MAX_LEN 16
struct ble_store_value_sec { ::ble_store_value_sec value; uint16_t bond_count; };
struct ble_store_value_cccd { uint8_t unused; };
struct ble_store_value_csfc { uint8_t unused; };
struct ble_store_value_ead { uint8_t unused; };
struct ble_store_value_local_irk { uint8_t unused; };
struct ble_store_value_rpa_rec { uint8_t unused; };
struct ble_hs_dev_records { uint8_t unused; };
union ble_store_value {
    ble_store_value_sec sec;
    ble_store_value_cccd cccd;
    ble_store_value_csfc csfc;
    ble_store_value_ead ead;
    ble_store_value_local_irk local_irk;
    ble_store_value_rpa_rec rpa_rec;
};
int loggedErrors = 0;
int failType = 0, failIndex = 0;
ble_store_value_sec ble_store_config_our_secs[3]{};
ble_store_value_sec ble_store_config_peer_secs[3]{};
ble_store_value_cccd ble_store_config_cccds[3]{};
ble_store_value_csfc ble_store_config_csfcs[3]{};
ble_store_value_ead ble_store_config_eads[3]{};
int ble_store_config_num_eads = 0;
ble_store_value_local_irk ble_store_config_local_irks[3]{};
ble_store_value_rpa_rec ble_store_config_rpa_recs[3]{};
ble_hs_dev_records peerRecords[3]{};
int ble_store_config_num_our_secs = 0, ble_store_config_num_peer_secs = 0;
int ble_store_config_num_cccds = 0, ble_store_config_num_csfcs = 0;
int ble_store_config_num_local_irks = 0, ble_store_config_num_rpa_recs = 0;
uint16_t ble_store_config_our_bond_count = 0, ble_store_config_peer_bond_count = 0;
std::vector<::ble_store_value_sec> durableOur, durablePeer;
int ble_store_config_compare_bond_count(const void* a, const void* b) {
    return int(static_cast<const ble_store_value_sec*>(a)->bond_count) -
           int(static_cast<const ble_store_value_sec*>(b)->bond_count);
}
int get_nvs_max_obj_value(int) { return 3; }
int currentReadType = 0, currentReadIndex = 0;
int faultStage = 0; // 0=size query, 1=open, 2=payload read
constexpr const char* NIMBLE_NVS_NAMESPACE = "nimble_bond";
constexpr int BLE_HS_ESTORE_FAIL = 31;
constexpr int NVS_READWRITE = 1;
void get_nvs_key_string(int type, int index, char* key) {
    currentReadType = type;
    currentReadIndex = index;
    std::snprintf(key, 16, "entry_%d", index);
}
bool readFailsAt(int stage) {
    return currentReadType == failType && currentReadIndex == failIndex && faultStage == stage;
}
int nvs_open(const char*, int, nvs_handle_t* handle) {
    if (readFailsAt(1)) return ESP_FAIL;
    *handle = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
int nvs_get_blob(nvs_handle_t, const char*, void* value, size_t* size) {
    if (readFailsAt(value ? 2 : 0)) return ESP_FAIL;
    const auto* entries = currentReadType == BLE_STORE_OBJ_TYPE_OUR_SEC ? &durableOur :
                          currentReadType == BLE_STORE_OBJ_TYPE_PEER_SEC ? &durablePeer : nullptr;
    if (!entries || size_t(currentReadIndex) > entries->size()) return ESP_ERR_NVS_NOT_FOUND;
    ble_store_value_sec entry{(*entries)[currentReadIndex - 1], static_cast<uint16_t>(currentReadIndex)};
    if (value) {
        TEST_ASSERT_GREATER_OR_EQUAL_UINT(sizeof(entry), *size);
        std::memcpy(value, &entry, sizeof(entry));
    }
    *size = sizeof(entry);
    return ESP_OK;
}
ble_hs_dev_records* ble_rpa_get_peer_dev_records() { return peerRecords; }
void ble_rpa_set_num_peer_dev_records(int) {}
#include "pinned_loader.inc"
void initialize() {
    loggedErrors = 0;
    std::memset(ble_store_config_our_secs, 0, sizeof(ble_store_config_our_secs));
    std::memset(ble_store_config_peer_secs, 0, sizeof(ble_store_config_peer_secs));
    ble_store_config_num_our_secs = ble_store_config_num_peer_secs = 0;
    ble_store_config_num_cccds = ble_store_config_num_csfcs = 0;
    ble_store_config_num_eads = 0;
    ble_store_config_num_local_irks = ble_store_config_num_rpa_recs = 0;
    ble_store_config_conf_init();
    gOurSecs.clear();
    gPeerSecs.clear();
    for (int i = 0; i < ble_store_config_num_our_secs; ++i)
        gOurSecs.push_back(ble_store_config_our_secs[i].value);
    for (int i = 0; i < ble_store_config_num_peer_secs; ++i)
        gPeerSecs.push_back(ble_store_config_peer_secs[i].value);
}
} // namespace PinnedNimble

extern "C" int v1simple_nimble_bond_bootstrap_status(void) {
    return PinnedNimble::v1simple_nimble_bond_bootstrap_status();
}
constexpr int BLE_TX_POWER_DBM = 9;
constexpr int BLE_GAP_LE_PHY_1M_MASK = 1, BLE_SM_IO_CAP_NO_IO = 0;
constexpr int BLE_SM_PAIR_KEY_DIST_ENC = 1, BLE_SM_PAIR_KEY_DIST_ID = 2;
constexpr int NIMBLE_CONN_INTERVAL_MIN = 12, NIMBLE_CONN_INTERVAL_MAX = 24;
constexpr int NIMBLE_CONN_LATENCY = 0, NIMBLE_CONN_SUPERVISION_TIMEOUT = 400;
constexpr int NIMBLE_CONNECT_TIMEOUT_INIT_MS = 5000;
int bondRecoveryCallbacks = 0;
int advertisingCalls = 0, clientCalls = 0, clearCalls = 0;
bool initFails = false;
struct ClientCallbacks {};
struct TestBleClient {
    void setClientCallbacks(ClientCallbacks*) {}
    void setConnectionParams(int, int, int, int) {}
    void setConnectTimeout(int) {}
    void setConnectRetries(int) {}
};
struct NimBLEDevice {
    static void setDeviceCallbacks(int*) {}
    static bool init(const char*) {
        if (initFails) return false;
        PinnedNimble::initialize();
        return true;
    }
    static void setDeviceName(const char*) {}
    static void setPower(int) {}
    static void setMTU(int) {}
    static void setDefaultPhy(int, int) {}
    static void setSecurityAuth(bool, bool, bool) {}
    static void setSecurityIOCap(int) {}
    static void setSecurityInitKey(int) {}
    static void setSecurityRespKey(int) {}
    // Pinned NimBLE's utility counts unique OUR_SEC addresses; ours are unique.
    static int getNumBonds() { return static_cast<int>(gOurSecs.size()); }
    static void deleteAllBonds() { ++clearCalls; gOurSecs.clear(); gPeerSecs.clear(); }
    static TestBleClient* createClient() { ++clientCalls; static TestBleClient client; return &client; }
};
class V1BLEClient {
  public:
    bool initBLE(StorageManager&, bool, const char*);
    bool proxyEnabled_ = false, proxyServerInitAttempted_ = false, proxyServerInitialized_ = false;
    String proxyName_;
    SemaphoreHandle_t bleMutex_ = nullptr, bleNotifyMutex_ = nullptr;
    uint8_t lastBondBackupCount_ = 0;
    TestBleClient* pClient_ = nullptr;
    std::unique_ptr<ClientCallbacks> pClientCallbacks_;
    bool initProxyServer(const char*) { ++advertisingCalls; return true; }
    void releaseProxyQueues() {}
};
#include "ble_init.inc"

int scenarioType = 0, scenarioIndex = 0;
std::string scenario;
std::string readBackupBytes() {
    std::ifstream input(bondPath(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
void seedCompleteBootBackup() {
    gOurSecs = {makeSec(1), makeSec(2)};
    gPeerSecs = {makeSec(21), makeSec(22)};
    PinnedNimble::durableOur = gOurSecs;
    PinnedNimble::durablePeer = gPeerSecs;
    TEST_ASSERT_EQUAL_INT(4, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
}
void test_bootstrap_admission_and_recovery() {
    TEST_ASSERT_NOT_EQUAL(0, v1simple_nimble_bond_bootstrap_status());
    seedCompleteBootBackup();
    const std::string completeBackup = readBackupBytes();
    PinnedNimble::failType = scenarioType;
    PinnedNimble::failIndex = scenarioIndex;
    PinnedNimble::faultStage = scenario == "open" ? 1 : scenario == "payload" ? 2 : 0;
    V1BLEClient client;
    if (scenarioType != 0 || scenario == "init-failure") {
        Preferences schema;
        TEST_ASSERT_TRUE(schema.begin(BleFreshFlashPolicy::kNamespace, false));
        schema.putUInt(BleFreshFlashPolicy::kBondSchemaVersionKey, 99);
        schema.end();
        initFails = scenario == "init-failure";
        TEST_ASSERT_FALSE(client.initBLE(storage, true, "Test"));
        TEST_ASSERT_EQUAL_INT(0, advertisingCalls);
        TEST_ASSERT_EQUAL_INT(0, clientCalls);
        TEST_ASSERT_EQUAL_INT(0, clearCalls);
        TEST_ASSERT_EQUAL_UINT(0, bleBondBackupQueueDepthForTest());
        TEST_ASSERT_TRUE(completeBackup == readBackupBytes());
        TEST_ASSERT_TRUE(schema.begin(BleFreshFlashPolicy::kNamespace, false));
        TEST_ASSERT_EQUAL_UINT32(99, schema.getUInt(BleFreshFlashPolicy::kBondSchemaVersionKey, 0));
        schema.putUInt(BleFreshFlashPolicy::kBondSchemaVersionKey, 1);
        schema.end();
        // A later boot reads all tables successfully; its result must replace
        // the failed bootstrap status without inheriting partial RAM records.
        PinnedNimble::failType = 0;
        PinnedNimble::failIndex = 0;
        initFails = false;
    } else if (scenario == "empty" || scenario == "our-only" || scenario == "peer-only") {
        if (scenario != "our-only") PinnedNimble::durableOur.clear();
        if (scenario != "peer-only") PinnedNimble::durablePeer.clear();
        std::filesystem::remove(bondPath());
    }
    TEST_ASSERT_TRUE(client.initBLE(storage, true, "Test"));
    TEST_ASSERT_EQUAL_INT(0, v1simple_nimble_bond_bootstrap_status());
    TEST_ASSERT_EQUAL_INT(1, advertisingCalls);
    TEST_ASSERT_EQUAL_INT(1, clientCalls);
    if (bleBondBackupQueueDepthForTest() != 0) TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    if (scenario != "empty" && scenario != "our-only" && scenario != "peer-only") {
        TEST_ASSERT_TRUE(completeBackup == readBackupBytes());
    }
    // Healthy new pairing and intentional deletion must retain ordinary backup
    // semantics. An empty snapshot is valid after complete initialization.
    gOurSecs = {makeSec(1), makeSec(2), makeSec(3)};
    gPeerSecs = {makeSec(21), makeSec(22), makeSec(23)};
    TEST_ASSERT_EQUAL_INT(6, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT32(3, readHeader().ourSecCount);
    TEST_ASSERT_EQUAL_UINT32(3, readHeader().peerSecCount);
    gOurSecs.clear();
    gPeerSecs.clear();
    TEST_ASSERT_EQUAL_INT(0, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT32(0, readHeader().ourSecCount);
    TEST_ASSERT_EQUAL_UINT32(0, readHeader().peerSecCount);
    // Library initialization can run again after deinitialization. A successful
    // prior result must not survive a later failed first read, and a successful
    // privacy-table read must not replace a failed security-table result.
    PinnedNimble::failType = BLE_STORE_OBJ_TYPE_OUR_SEC;
    PinnedNimble::failIndex = 1;
    PinnedNimble::faultStage = 0;
    PinnedNimble::initialize();
    TEST_ASSERT_NOT_EQUAL(0, v1simple_nimble_bond_bootstrap_status());
    TEST_ASSERT_GREATER_THAN(0, PinnedNimble::loggedErrors);
    PinnedNimble::failType = 0;
    PinnedNimble::initialize();
    TEST_ASSERT_EQUAL_INT(0, v1simple_nimble_bond_bootstrap_status());
}
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    scenario = argv[1];
    scenarioType = std::atoi(argv[2]);
    scenarioIndex = std::atoi(argv[3]);
    UNITY_BEGIN();
    RUN_TEST(test_bootstrap_admission_and_recovery);
    return UNITY_END();
}
