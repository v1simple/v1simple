/**
 * Regression boundary: BLE bond snapshots enter a nonblocking Core-0 writer,
 * coalesce when full, retry failures, and close cleanly around shutdown,
 * without filesystem work in runtime callers.
 */
#include <unity.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../mocks/Arduino.h"
#include "../mocks/FS.h"
#include "../mocks/Preferences.h"
#include "../mocks/nvs.h"
#include "../mocks/storage_manager.h"
#include "../../src/ble_bond_backup_store.h"

extern "C" {
#include "nimble/nimble/host/include/host/ble_store.h"
}

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

namespace {

std::filesystem::path gRoot;
fs::FS gFs;
std::vector<ble_store_value_sec> gOurSecs;
std::vector<ble_store_value_sec> gPeerSecs;
std::vector<ble_store_value_sec> gRestoredSecs;
// The pinned ESP NimBLE store mutates RAM before it attempts NVS persistence.
// Enable separate durable tables only for recovery lifecycle scenarios.
bool gModelBondPersistence = false;
std::vector<ble_store_value_sec> gDurableOurSecs;
std::vector<ble_store_value_sec> gDurablePeerSecs;
size_t gBondWriteCalls = 0;
size_t gFailBondWriteCall = 0;
size_t gResetAfterBondWriteCall = 0;
size_t gBondCapacity = 8;
size_t gOldestBondEvictions = 0;
struct BondRestoreReset {};

void upsertBond(std::vector<ble_store_value_sec>& entries, const ble_store_value_sec& sec) {
    for (auto& entry : entries) {
        if (memcmp(&entry.peer_addr, &sec.peer_addr, sizeof(sec.peer_addr)) == 0) {
            entry = sec;
            return;
        }
    }
    entries.push_back(sec);
}

int persistBond(const ble_store_value_sec* sec, bool ours) {
    ++gBondWriteCalls;
    if (gModelBondPersistence) {
        auto& current = ours ? gOurSecs : gPeerSecs;
        bool present = false;
        for (const auto& existing : current)
            if (memcmp(&existing.peer_addr, &sec->peer_addr, sizeof(sec->peer_addr)) == 0) present = true;
        if (!present && current.size() >= gBondCapacity) {
            if (handleBleBondStoreStatus(nullptr, nullptr) != 0) return 0x1f;
        }
        upsertBond(ours ? gOurSecs : gPeerSecs, *sec);
        if (gBondWriteCalls == gFailBondWriteCall) return 0x1e;
        upsertBond(ours ? gDurableOurSecs : gDurablePeerSecs, *sec);
        if (gBondWriteCalls == gResetAfterBondWriteCall) throw BondRestoreReset{};
    }
    gRestoredSecs.push_back(*sec);
    return 0;
}


ble_store_value_sec makeSec(uint8_t base) {
    ble_store_value_sec sec = {};
    for (size_t i = 0; i < sizeof(sec.peer_addr); ++i) {
        sec.peer_addr[i] = static_cast<uint8_t>(base + i);
    }
    for (size_t i = 0; i < sizeof(sec.payload); ++i) {
        sec.payload[i] = static_cast<uint8_t>(base + 10 + i);
    }
    return sec;
}

std::filesystem::path bondPath() {
    return gRoot / "v1simple_ble_bonds.bin";
}

BondBackupHeader readHeader() {
    std::ifstream input(bondPath(), std::ios::binary);
    TEST_ASSERT_TRUE(input.is_open());
    BondBackupHeader header = {};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    TEST_ASSERT_EQUAL_UINT(sizeof(header), static_cast<size_t>(input.gcount()));
    return header;
}

uint8_t readFirstOurAddressByte() {
    std::ifstream input(bondPath(), std::ios::binary);
    TEST_ASSERT_TRUE(input.is_open());
    input.seekg(static_cast<std::streamoff>(sizeof(BondBackupHeader)), std::ios::beg);
    ble_store_value_sec sec = {};
    input.read(reinterpret_cast<char*>(&sec), sizeof(sec));
    TEST_ASSERT_EQUAL_UINT(sizeof(sec), static_cast<size_t>(input.gcount()));
    return sec.peer_addr[0];
}

std::string readProjectFile(const char* relativePath) {
    const std::filesystem::path path =
        std::filesystem::path(PROJECT_DIR) / relativePath;
    std::ifstream input(path);
    TEST_ASSERT_TRUE_MESSAGE(input.is_open(), path.string().c_str());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::string functionBody(const std::string& source, const std::string& signature) {
    const size_t signaturePos = source.find(signature);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, signaturePos, signature.c_str());
    const size_t open = source.find('{', signaturePos);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, open);

    int depth = 0;
    for (size_t pos = open; pos < source.size(); ++pos) {
        if (source[pos] == '{') {
            ++depth;
        } else if (source[pos] == '}') {
            --depth;
            if (depth == 0) {
                return source.substr(open, pos - open + 1);
            }
        }
    }
    TEST_FAIL_MESSAGE("unterminated function body");
    return {};
}

}  // namespace

extern "C" int ble_store_util_status_rr(ble_store_status_event*, void*) {
    ++gOldestBondEvictions;
    if (!gOurSecs.empty()) gOurSecs.erase(gOurSecs.begin());
    return 0;
}

extern "C" int ble_store_iterate(int objType,
                                  ble_store_iterator_fn callback,
                                  void* cookie) {
    const std::vector<ble_store_value_sec>& source =
        objType == BLE_STORE_OBJ_TYPE_OUR_SEC ? gOurSecs : gPeerSecs;
    for (const ble_store_value_sec& sec : source) {
        ble_store_value value = {};
        value.sec = sec;
        if (callback(objType, &value, cookie) != 0) {
            break;
        }
    }
    return 0;
}

extern "C" int ble_store_write_our_sec(const ble_store_value_sec* sec) {
    return persistBond(sec, true);
}

extern "C" int ble_store_write_peer_sec(const ble_store_value_sec* sec) {
    return persistBond(sec, false);
}

#include "../../src/psram_freertos_alloc.cpp"
#include "../../src/ble_bond_backup_store.cpp"
#include "../../src/ble_bond_backup_writer.cpp"

void setUp() {
    gRoot = std::filesystem::temp_directory_path() / "v1simple_ble_bond_writer";
    std::error_code ec;
    std::filesystem::remove_all(gRoot, ec);
    std::filesystem::create_directories(gRoot, ec);
    gFs = fs::FS(gRoot);
    gOurSecs.clear();
    gPeerSecs.clear();
    gRestoredSecs.clear();
    gModelBondPersistence = false;
    gDurableOurSecs.clear();
    gDurablePeerSecs.clear();
    gBondWriteCalls = gFailBondWriteCall = gResetAfterBondWriteCall = 0;
    gBondCapacity = 8;
    gOldestBondEvictions = 0;
    mock_preferences::reset();
    mock_nvs::reset();
    initializeBleBondRecovery();
    fs::mock_reset_fs_rename_state();
    fs::mock_reset_fs_open_state();
    fs::mock_reset_fs_remove_state();
    storage.reset();
    storage.setFilesystem(&gFs, true);
    StorageManager::resetMockSdLockState();
    mock_reset_heap_caps();
    mock_reset_queue_create_state();
    mock_reset_task_create_state();
    resetBleBondBackupWriterForTest();
    registerBleBondBackupStorage(storage);
}

void tearDown() {
    resetBleBondBackupWriterForTest();
    std::error_code ec;
    std::filesystem::remove_all(gRoot, ec);
}

void test_runtime_enqueue_is_nonblocking_and_writer_is_pinned_to_core_zero() {
    gOurSecs = {makeSec(1)};
    gPeerSecs = {makeSec(21)};

    TEST_ASSERT_EQUAL(2, enqueueCurrentBleBondBackupSnapshot());

    TEST_ASSERT_EQUAL_UINT(1, bleBondBackupQueueDepthForTest());
    TEST_ASSERT_FALSE(std::filesystem::exists(bondPath()));
    TEST_ASSERT_EQUAL_UINT32(0, StorageManager::mockSdLockState.blockingAcquireCalls);
    TEST_ASSERT_EQUAL_UINT32(0, StorageManager::mockSdLockState.tryAcquireCalls);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_task_create_state.capsCalls);
    TEST_ASSERT_EQUAL_INT(0, g_mock_task_create_state.lastCore);
    TEST_ASSERT_EQUAL_UINT(sizeof(void*), g_mock_queue_create_state.lastItemSize);
    TEST_ASSERT_TRUE(bleBondBackupSnapshotSizeForTest() <= 2048);

    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT32(1, StorageManager::mockSdLockState.blockingAcquireCalls);
    const BondBackupHeader header = readHeader();
    TEST_ASSERT_EQUAL_UINT32(1, header.ourSecCount);
    TEST_ASSERT_EQUAL_UINT32(1, header.peerSecCount);

}

void test_zero_bond_snapshot_replaces_stale_backup_after_auto_heal() {
    gOurSecs = {makeSec(8)};
    TEST_ASSERT_EQUAL(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT32(1, readHeader().ourSecCount);

    gOurSecs.clear();
    gPeerSecs.clear();
    TEST_ASSERT_EQUAL(0, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());

    const BondBackupHeader header = readHeader();
    TEST_ASSERT_EQUAL_UINT32(0, header.ourSecCount);
    TEST_ASSERT_EQUAL_UINT32(0, header.peerSecCount);
    TEST_ASSERT_EQUAL_UINT(sizeof(BondBackupHeader),
                           std::filesystem::file_size(bondPath()));
}

void test_full_queue_coalesces_to_latest_snapshot_without_waiting() {
    gOurSecs = {makeSec(3)};
    TEST_ASSERT_EQUAL(1, enqueueCurrentBleBondBackupSnapshot());

    gOurSecs = {makeSec(31)};
    TEST_ASSERT_EQUAL(1, enqueueCurrentBleBondBackupSnapshot());

    TEST_ASSERT_EQUAL_UINT(1, bleBondBackupQueueDepthForTest());
    TEST_ASSERT_EQUAL_UINT32(0, StorageManager::mockSdLockState.blockingAcquireCalls);
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT8(31, readFirstOurAddressByte());

}

void test_writer_failure_retains_snapshot_for_retry() {
    gOurSecs = {makeSec(44)};
    TEST_ASSERT_EQUAL(1, enqueueCurrentBleBondBackupSnapshot());

    storage.reset();
    TEST_ASSERT_FALSE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT(1, bleBondBackupQueueDepthForTest());

    storage.setFilesystem(&gFs, true);
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT8(44, readFirstOurAddressByte());

}

void test_writer_setup_failure_releases_snapshot_without_sd_access() {
    gOurSecs = {makeSec(55)};
    g_mock_task_create_state.failCaps = true;

    TEST_ASSERT_EQUAL(-1, enqueueCurrentBleBondBackupSnapshot());

    TEST_ASSERT_EQUAL_UINT(0, bleBondBackupQueueDepthForTest());
    TEST_ASSERT_EQUAL_UINT32(0, StorageManager::mockSdLockState.blockingAcquireCalls);
}

void test_aborted_shutdown_reopens_bond_backup_writer_admission() {
    gOurSecs = {makeSec(67)};
    shutdownBleBondBackupWriter(0);

    TEST_ASSERT_EQUAL(-1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_EQUAL_UINT(0, bleBondBackupQueueDepthForTest());

    resumeBleBondBackupWriterAfterAbortedShutdown();

    TEST_ASSERT_EQUAL(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_EQUAL_UINT(1, bleBondBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT8(67, readFirstOurAddressByte());
}

void test_resume_handoffs_snapshot_queued_while_old_bond_writer_exits() {
    gOurSecs = {makeSec(71)};
    TEST_ASSERT_EQUAL(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_task_create_state.capsCalls);

    // Model the bounded-timeout boundary: the old task has committed to exit,
    // abort recovery reopens admission, and an event queues a snapshot before
    // the old task publishes itself inactive.
    gBondBackupWriterState.shutdownRequested.store(true, std::memory_order_release);
    resumeBleBondBackupWriterAfterAbortedShutdown();
    gOurSecs = {makeSec(79)};
    TEST_ASSERT_EQUAL(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_task_create_state.capsCalls);
    TEST_ASSERT_EQUAL_UINT(1, bleBondBackupQueueDepthForTest());

    TEST_ASSERT_TRUE(completeBondBackupWriterExit());

    TEST_ASSERT_EQUAL_UINT32(1, g_mock_task_create_state.capsCalls);
    TEST_ASSERT_TRUE(gBondBackupWriterState.writerActive.load(std::memory_order_acquire));
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT8(79, readFirstOurAddressByte());
}

void test_shutdown_exit_drains_late_bond_snapshot_and_waits_for_admissions() {
    gOurSecs = {makeSec(83)};
    TEST_ASSERT_EQUAL(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_EQUAL_UINT(1, bleBondBackupQueueDepthForTest());

    gBondBackupWriterState.writerActive.store(false, std::memory_order_release);
    TEST_ASSERT_FALSE(bondBackupWriterQuiesced());
    gBondBackupWriterState.writerActive.store(true, std::memory_order_release);

    gBondBackupWriterState.shutdownRequested.store(true, std::memory_order_release);
    TEST_ASSERT_TRUE(completeBondBackupWriterExit());
    TEST_ASSERT_TRUE(gBondBackupWriterState.writerActive.load(std::memory_order_acquire));
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_FALSE(completeBondBackupWriterExit());
    TEST_ASSERT_TRUE(bondBackupWriterQuiesced());
    TEST_ASSERT_EQUAL_UINT8(83, readFirstOurAddressByte());

    gBondBackupWriterState.writerAdmissionsInFlight.store(1, std::memory_order_release);
    TEST_ASSERT_FALSE(bondBackupWriterQuiesced());
    gBondBackupWriterState.writerAdmissionsInFlight.store(0, std::memory_order_release);
    TEST_ASSERT_TRUE(bondBackupWriterQuiesced());
}

void test_runtime_callers_contain_no_sd_lock_or_filesystem_write() {
    const std::string client = readProjectFile("src/ble_client.cpp");
    const std::string followup = readProjectFile("src/ble_connected_followup.cpp");
    const std::string obd = readProjectFile("src/modules/obd/obd_runtime_commands.cpp");
    const std::string writer = readProjectFile("src/ble_bond_backup_writer.cpp");

    const std::string refresh = functionBody(client, "int refreshBleBondBackup()");
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          refresh.find("enqueueCurrentBleBondBackupSnapshot"));
    TEST_ASSERT_EQUAL(std::string::npos, refresh.find("SDLockBlocking"));
    TEST_ASSERT_EQUAL(std::string::npos, refresh.find("SDTryLock"));
    TEST_ASSERT_EQUAL(std::string::npos, refresh.find("getFilesystem"));

    const std::string service = functionBody(
        followup, "void V1BLEClient::serviceDeferredBondBackup(uint32_t nowMs)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          service.find("enqueueCurrentBondBackupSnapshot"));
    TEST_ASSERT_EQUAL(std::string::npos, service.find("SDLockBlocking"));
    TEST_ASSERT_EQUAL(std::string::npos, service.find("SDTryLock"));
    TEST_ASSERT_EQUAL(std::string::npos, service.find("getFilesystem"));

    const std::string autoHeal = functionBody(
        obd, "bool ObdRuntimeModule::autoHealBondIfAllowed(uint32_t nowMs, const char* context)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, autoHeal.find("refreshBleBondBackup"));
    TEST_ASSERT_EQUAL(std::string::npos, autoHeal.find("SDLockBlocking"));
    TEST_ASSERT_EQUAL(std::string::npos, autoHeal.find("getFilesystem"));

    const std::string bootBackup = functionBody(
        writer, "int backupCurrentBleBondsViaCore0AtBoot()");
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          bootBackup.find("enqueueCurrentBondSnapshotImpl"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          bootBackup.find("kBootBackupWaitTimeoutMs"));
    TEST_ASSERT_EQUAL(std::string::npos, bootBackup.find("SDLockBlocking"));
    TEST_ASSERT_EQUAL(std::string::npos, bootBackup.find("getFilesystem"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      bootBackup.find("writeBondBackupSnapshotWithSdLock"));

    const std::string writerTask = functionBody(
        writer, "void bondBackupWriterTaskEntry(void* context)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          writerTask.find("vTaskDeleteWithCaps(nullptr)"));
}


namespace {
constexpr const char* kTestBondPath = "/v1simple_ble_bonds.bin";
constexpr const char* kTestBondPrevious = "/v1simple_ble_bonds.bin.prev";

void seedBondRecovery(bool corruptLive) {
    gOurSecs = {makeSec(5)};
    TEST_ASSERT_EQUAL_INT(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    // A cleanup failure after a successful public replacement legitimately
    // leaves both generations. Corrupt only the newer live afterward.
    gOurSecs = {makeSec(7)};
    fs::mock_fail_next_remove(kTestBondPrevious);
    TEST_ASSERT_EQUAL_INT(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_TRUE(gFs.exists(kTestBondPrevious));
    if (corruptLive) {
        File live = gFs.open(kTestBondPath, FILE_WRITE);
        TEST_ASSERT_TRUE(bool(live));
        TEST_ASSERT_EQUAL_UINT(3, live.write(reinterpret_cast<const uint8_t*>("bad"), 3));
        live.close();
    }
    fs::mock_reset_fs_rename_state();
}

void expectRestoredBond(uint8_t expected) {
    gRestoredSecs.clear();
    TEST_ASSERT_EQUAL_INT(1, restoreBleBondBackup(gFs, kTestBondPath));
    TEST_ASSERT_EQUAL_UINT(1, gRestoredSecs.size());
    TEST_ASSERT_EQUAL_UINT8(expected, gRestoredSecs[0].peer_addr[0]);
}

void rebootBondOwner() {
    initializeBleBondRecovery();
    resetBleBondBackupWriterForTest();
    registerBleBondBackupStorage(storage);
    fs::mock_reset_fs_rename_state();
    fs::mock_reset_fs_remove_state();
    fs::mock_reset_fs_open_state();
}

void failBondReplacement(bool corruptLive, bool missingLive, bool failedRollback) {
    seedBondRecovery(corruptLive);
    if (missingLive) TEST_ASSERT_TRUE(gFs.remove(kTestBondPath));
    expectRestoredBond(corruptLive || missingLive ? 5 : 7);
    gOurSecs = {makeSec(20)};
    TEST_ASSERT_EQUAL_INT(1, enqueueCurrentBleBondBackupSnapshot());
    const size_t promotionCall = corruptLive || missingLive ? 1 : 2;
    fs::mock_fail_rename_on_calls(promotionCall, failedRollback ? promotionCall + 1 : 0);
    TEST_ASSERT_FALSE(runBleBondBackupWriterOnceForTest());
    expectRestoredBond(corruptLive || missingLive ? 5 : 7);
    TEST_ASSERT_EQUAL_UINT(1, bleBondBackupQueueDepthForTest());
    rebootBondOwner();
    expectRestoredBond(corruptLive || missingLive ? 5 : 7);
    // The next public writer can still replace the preserved recovery copy.
    TEST_ASSERT_EQUAL_INT(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    expectRestoredBond(20);
}

void resetBondReplacement(bool corruptLive, bool afterNormalization, size_t renameCall) {
    seedBondRecovery(corruptLive);
    gOurSecs = {makeSec(20)};
    BondBackupSnapshot snapshot;
    collectCurrentBondSnapshot(snapshot);
    if (afterNormalization) fs::mock_reset_after_remove(kTestBondPath);
    else fs::mock_reset_after_rename_on_call(renameCall);
    bool reset = false;
    try { (void)writeBondBackupSnapshot(gFs, snapshot); }
    catch (const fs::SimulatedReset&) { reset = true; }
    TEST_ASSERT_TRUE(reset);
    rebootBondOwner();
    const bool committed = !afterNormalization && (corruptLive || renameCall == 2);
    expectRestoredBond(committed ? 20 : corruptLive ? 5 : 7);
}
} // namespace

void test_corrupt_live_failed_bond_promotion_preserves_previous_through_reboot_and_retry() {
    failBondReplacement(true, false, false);
}
void test_missing_live_failed_bond_promotion_preserves_previous_through_reboot_and_retry() {
    failBondReplacement(false, true, false);
}
void test_healthy_live_failed_bond_promotion_restores_latest_through_reboot_and_retry() {
    failBondReplacement(false, false, false);
}
void test_failed_bond_promotion_and_rollback_preserve_latest_through_reboot_and_retry() {
    failBondReplacement(false, false, true);
}
void test_reset_after_corrupt_bond_live_removal_preserves_previous() { resetBondReplacement(true, true, 0); }
void test_reset_after_valid_bond_live_rotation_preserves_latest() { resetBondReplacement(false, false, 1); }
void test_reset_after_recovered_bond_promotion_selects_new_commit() { resetBondReplacement(true, false, 1); }
void test_reset_after_healthy_bond_promotion_selects_new_commit() { resetBondReplacement(false, false, 2); }

void test_bond_live_remove_failure_preserves_both_recovery_candidates() {
    seedBondRecovery(true);
    gOurSecs = {makeSec(20)};
    fs::mock_fail_next_remove(kTestBondPath);
    TEST_ASSERT_EQUAL_INT(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_FALSE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT(0, fs::g_mock_fs_rename_state.renameCalls);
    rebootBondOwner();
    expectRestoredBond(5);
}

void test_unreadable_live_bond_backup_does_not_rotate_or_overwrite_recovery() {
    seedBondRecovery(false);
    gOurSecs = {makeSec(20)};
    fs::mock_fail_next_read_open(kTestBondPath);
    TEST_ASSERT_EQUAL_INT(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_FALSE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT(0, fs::g_mock_fs_rename_state.renameCalls);
    rebootBondOwner();
    expectRestoredBond(7);
}

void test_unreadable_previous_bond_backup_does_not_replace_it_with_corrupt_live() {
    seedBondRecovery(true);
    gOurSecs = {makeSec(20)};
    fs::mock_fail_next_read_open(kTestBondPrevious);
    TEST_ASSERT_EQUAL_INT(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_FALSE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT(0, fs::g_mock_fs_rename_state.renameCalls);
    rebootBondOwner();
    expectRestoredBond(5);
}

void test_bond_candidate_validation_failure_preserves_committed_backup_and_retries() {
    seedBondRecovery(false);
    gOurSecs = {makeSec(20)};
    fs::mock_fail_next_read_open("/v1simple_ble_bonds.bin.tmp");
    TEST_ASSERT_EQUAL_INT(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_FALSE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT(0, fs::g_mock_fs_rename_state.renameCalls);
    // Inspect without starting a recovery: recovery intentionally invalidates
    // snapshots captured before it, whereas this case exercises writer retry.
    TEST_ASSERT_EQUAL_UINT8(7, readFirstOurAddressByte());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    expectRestoredBond(20);
}

void test_bond_validation_never_changes_the_live_nimble_store() {
    seedBondRecovery(false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BleBondBackupStatus::Valid),
                          static_cast<int>(inspectBleBondBackup(gFs, kTestBondPath)));
    TEST_ASSERT_TRUE(gRestoredSecs.empty());
}

void test_unavailable_cached_file_size_cannot_demote_the_latest_bond_backup() {
    seedBondRecovery(false);
    gOurSecs = {makeSec(20)};
    fs::mock_zero_file_size_for_full_path(bondPath().c_str());
    TEST_ASSERT_EQUAL_INT(1, enqueueCurrentBleBondBackupSnapshot());
    fs::mock_fail_rename_on_call(1);
    TEST_ASSERT_FALSE(runBleBondBackupWriterOnceForTest());
    rebootBondOwner();
    expectRestoredBond(7);
    TEST_ASSERT_EQUAL_INT(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    expectRestoredBond(20);
}

namespace {

void clearBondNvsForRestore() {
    gOurSecs.clear();
    gPeerSecs.clear();
    gRestoredSecs.clear();
}

void expectRestoreAndNextBootSnapshot(uint8_t expected) {
    rebootBondOwner();
    expectRestoredBond(expected);
    // The restore call above reaches the real NimBLE write boundary. Model
    // those entries persisting in NVS, as seen by normal boot's snapshot.
    gOurSecs = gRestoredSecs;
    rebootBondOwner();
    TEST_ASSERT_EQUAL_INT(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT8(expected, readFirstOurAddressByte());
    clearBondNvsForRestore();
    rebootBondOwner();
    expectRestoredBond(expected);
}

void expectUnavailableRestorePreservesLatest() {
    TEST_ASSERT_EQUAL_INT(-1, restoreBleBondBackup(gFs, kTestBondPath));
    TEST_ASSERT_TRUE(gRestoredSecs.empty());
    TEST_ASSERT_TRUE(gOurSecs.empty());
    TEST_ASSERT_TRUE(gPeerSecs.empty());
    TEST_ASSERT_EQUAL_UINT(0, bleBondBackupQueueDepthForTest());
    TEST_ASSERT_EQUAL_UINT8(7, readFirstOurAddressByte());
    TEST_ASSERT_TRUE(gFs.exists(kTestBondPrevious));
    expectRestoreAndNextBootSnapshot(7);
}

} // namespace

void test_restore_open_failure_never_seeds_previous_bonds_into_nvs() {
    seedBondRecovery(false);
    clearBondNvsForRestore();
    fs::mock_fail_next_read_open(kTestBondPath);
    expectUnavailableRestorePreservesLatest();
}

void test_restore_short_header_or_our_entry_read_never_seeds_previous_bonds_into_nvs() {
    for (const size_t readable : {size_t(0), sizeof(BondBackupHeader) - 1,
                                  sizeof(BondBackupHeader), sizeof(BondBackupHeader) + 1}) {
        setUp();
        seedBondRecovery(false);
        clearBondNvsForRestore();
        fs::mock_set_read_budget_for_full_path(bondPath().c_str(), readable);
        expectUnavailableRestorePreservesLatest();
    }
}

void test_restore_short_peer_entry_read_writes_no_partially_read_bonds() {
    gOurSecs = {makeSec(5)};
    TEST_ASSERT_EQUAL_INT(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    gOurSecs = {makeSec(7)};
    gPeerSecs = {makeSec(17)};
    fs::mock_fail_next_remove(kTestBondPrevious);
    TEST_ASSERT_EQUAL_INT(2, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    clearBondNvsForRestore();
    fs::mock_set_read_budget_for_full_path(
        bondPath().c_str(), sizeof(BondBackupHeader) + sizeof(ble_store_value_sec) + 1);

    TEST_ASSERT_EQUAL_INT(-1, restoreBleBondBackup(gFs, kTestBondPath));
    TEST_ASSERT_TRUE(gRestoredSecs.empty());
    TEST_ASSERT_EQUAL_UINT8(7, readFirstOurAddressByte());
    rebootBondOwner();
    TEST_ASSERT_EQUAL_INT(2, restoreBleBondBackup(gFs, kTestBondPath));
    TEST_ASSERT_EQUAL_UINT(2, gRestoredSecs.size());
    TEST_ASSERT_EQUAL_UINT8(7, gRestoredSecs[0].peer_addr[0]);
    TEST_ASSERT_EQUAL_UINT8(17, gRestoredSecs[1].peer_addr[0]);
}

void test_restore_unavailable_live_metadata_does_not_select_readable_previous() {
    seedBondRecovery(false);
    clearBondNvsForRestore();
    const auto heldLive = gRoot / "held-live.bin";
    std::filesystem::rename(bondPath(), heldLive);
    // A loop makes stat fail with ELOOP while .prev remains fully readable.
    std::filesystem::create_symlink(bondPath().filename(), bondPath());
    TEST_ASSERT_EQUAL_INT(-1, restoreBleBondBackup(gFs, kTestBondPath));
    TEST_ASSERT_TRUE(gRestoredSecs.empty());
    std::filesystem::remove(bondPath());
    std::filesystem::rename(heldLive, bondPath());
    expectRestoreAndNextBootSnapshot(7);
}

void test_restore_readable_live_remains_latest_after_normal_boot_backup() {
    seedBondRecovery(false);
    clearBondNvsForRestore();
    expectRestoreAndNextBootSnapshot(7);
}

void test_restore_missing_live_still_uses_previous_through_normal_boot_backup() {
    seedBondRecovery(false);
    TEST_ASSERT_TRUE(gFs.remove(kTestBondPath));
    clearBondNvsForRestore();
    expectRestoreAndNextBootSnapshot(5);
}

void test_restore_corrupt_live_still_uses_previous_through_normal_boot_backup() {
    seedBondRecovery(true);
    clearBondNvsForRestore();
    expectRestoreAndNextBootSnapshot(5);
}

void test_restore_valid_empty_live_does_not_resurrect_previous_bonds() {
    gOurSecs = {makeSec(5)};
    TEST_ASSERT_EQUAL_INT(1, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    clearBondNvsForRestore();
    fs::mock_fail_next_remove(kTestBondPrevious);
    TEST_ASSERT_EQUAL_INT(0, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_TRUE(gFs.exists(kTestBondPrevious));
    TEST_ASSERT_EQUAL_INT(0, restoreBleBondBackup(gFs, kTestBondPath));
    TEST_ASSERT_TRUE(gRestoredSecs.empty());
}

namespace {
void seedCompleteRecovery(bool previous = false) {
    gOurSecs = {makeSec(1), makeSec(11)};
    gPeerSecs = {makeSec(21)};
    TEST_ASSERT_EQUAL_INT(3, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    if (previous) TEST_ASSERT_TRUE(gFs.rename(kTestBondPath, kTestBondPrevious));
    clearBondNvsForRestore();
    gModelBondPersistence = true;
}

void bootRecoveryOwner() {
    gOurSecs = gDurableOurSecs;
    gPeerSecs = gDurablePeerSecs;
    gBondWriteCalls = gFailBondWriteCall = gResetAfterBondWriteCall = 0;
    mock_preferences::afterWrite() = {};
    mock_preferences::set_fail_writes_for_key(nullptr);
    mock_nvs::reset();
    rebootBondOwner();
    restoreOrBackupBleBondsAtBoot(storage, gOurSecs.size());
}

void expectCompleteRecoveredBackup() {
    TEST_ASSERT_FALSE(bleBondRecoveryPending());
    TEST_ASSERT_EQUAL_UINT(2, gDurableOurSecs.size());
    TEST_ASSERT_EQUAL_UINT(1, gDurablePeerSecs.size());
    bootRecoveryOwner();
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT32(2, readHeader().ourSecCount);
    TEST_ASSERT_EQUAL_UINT32(1, readHeader().peerSecCount);
}
}

void test_interrupted_restore_resumes_after_each_durable_record_without_replacing_backup() {
    for (size_t interrupted = 1; interrupted <= 3; ++interrupted) {
        setUp();
        seedCompleteRecovery();
        // A stale queued snapshot is also denied once recovery has started.
        TEST_ASSERT_EQUAL_INT(0, enqueueCurrentBleBondBackupSnapshot());
        gResetAfterBondWriteCall = interrupted;
        bool reset = false;
        try { restoreOrBackupBleBondsAtBoot(storage, 0); }
        catch (const BondRestoreReset&) { reset = true; }
        TEST_ASSERT_TRUE(reset);
        TEST_ASSERT_TRUE(bleBondRecoveryPending());
        TEST_ASSERT_EQUAL_INT(-1, enqueueCurrentBleBondBackupSnapshot());
        TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
        TEST_ASSERT_EQUAL_UINT(0, bleBondBackupQueueDepthForTest());
        TEST_ASSERT_EQUAL_UINT32(2, readHeader().ourSecCount);
        TEST_ASSERT_EQUAL_UINT32(1, readHeader().peerSecCount);
        bootRecoveryOwner();
        TEST_ASSERT_EQUAL_UINT(3 - interrupted, gBondWriteCalls);
        expectCompleteRecoveredBackup();
    }
}

void test_failed_our_or_peer_write_keeps_recovery_pending_until_fresh_boot() {
    for (const size_t failed : {size_t(2), size_t(3)}) {
        setUp();
        seedCompleteRecovery();
        gFailBondWriteCall = failed;
        restoreOrBackupBleBondsAtBoot(storage, 0);
        TEST_ASSERT_TRUE(bleBondRecoveryPending());
        TEST_ASSERT_EQUAL_INT(-1, restoreBleBondBackup(gFs, kTestBondPath));
        TEST_ASSERT_EQUAL_UINT(failed, gBondWriteCalls);
        TEST_ASSERT_EQUAL_INT(-1, enqueueCurrentBleBondBackupSnapshot());
        bootRecoveryOwner();
        expectCompleteRecoveredBackup();
    }
}

void test_recovery_marker_failure_does_not_start_nimble_writes() {
    seedCompleteRecovery();
    mock_preferences::set_fail_writes_for_key(kBondRecoveryKey);
    restoreOrBackupBleBondsAtBoot(storage, 0);
    TEST_ASSERT_TRUE(bleBondRecoveryPending());
    TEST_ASSERT_EQUAL_UINT(0, gBondWriteCalls);
    TEST_ASSERT_EQUAL_INT(-1, enqueueCurrentBleBondBackupSnapshot());
    bootRecoveryOwner();
    expectCompleteRecoveredBackup();
}

void test_reset_after_recovery_marker_before_first_bond_write_resumes() {
    seedCompleteRecovery();
    mock_preferences::afterWrite() = [](const char* key) {
        if (std::string(key) == kBondRecoveryKey) throw BondRestoreReset{};
    };
    bool reset = false;
    try { restoreOrBackupBleBondsAtBoot(storage, 0); }
    catch (const BondRestoreReset&) { reset = true; }
    TEST_ASSERT_TRUE(reset);
    TEST_ASSERT_EQUAL_UINT(0, gBondWriteCalls);
    bootRecoveryOwner();
    expectCompleteRecoveredBackup();
}

void test_recovery_marker_read_or_type_error_defers_recovery_and_snapshots() {
    for (const int error : {-1, ESP_ERR_NVS_TYPE_MISMATCH}) {
        setUp();
        seedCompleteRecovery();
        mock_nvs::set_uint32_probe_result(kBondRecoveryNamespace, kBondRecoveryKey, error);
        initializeBleBondRecovery();
        restoreOrBackupBleBondsAtBoot(storage, 1);
        TEST_ASSERT_TRUE(bleBondRecoveryPending());
        TEST_ASSERT_EQUAL_UINT(0, gBondWriteCalls);
        TEST_ASSERT_EQUAL_INT(-1, enqueueCurrentBleBondBackupSnapshot());
        bootRecoveryOwner();
        expectCompleteRecoveredBackup();
    }
}

void test_recovery_marker_verification_failure_does_not_start_nimble_writes() {
    seedCompleteRecovery();
    mock_preferences::afterWrite() = [](const char* key) {
        if (std::string(key) == kBondRecoveryKey)
            mock_nvs::set_uint32_probe_result(kBondRecoveryNamespace, kBondRecoveryKey, -1);
    };
    restoreOrBackupBleBondsAtBoot(storage, 0);
    TEST_ASSERT_TRUE(bleBondRecoveryPending());
    TEST_ASSERT_EQUAL_UINT(0, gBondWriteCalls);
    bootRecoveryOwner();
    expectCompleteRecoveredBackup();
}

void test_recovery_marker_clear_failure_keeps_complete_source_until_next_boot() {
    seedCompleteRecovery();
    mock_preferences::set_fail_write_for_key_on_call(kBondRecoveryKey, 2);
    restoreOrBackupBleBondsAtBoot(storage, 0);
    TEST_ASSERT_TRUE(bleBondRecoveryPending());
    TEST_ASSERT_EQUAL_UINT(3, gBondWriteCalls);
    TEST_ASSERT_EQUAL_INT(-1, enqueueCurrentBleBondBackupSnapshot());
    bootRecoveryOwner();
    TEST_ASSERT_EQUAL_UINT(0, gBondWriteCalls);
    expectCompleteRecoveredBackup();
}

void test_pending_restore_waits_for_absent_or_unreadable_sd_without_losing_bonds() {
    seedCompleteRecovery();
    gFailBondWriteCall = 2;
    restoreOrBackupBleBondsAtBoot(storage, 0);
    storage.reset();
    bootRecoveryOwner();
    TEST_ASSERT_TRUE(bleBondRecoveryPending());
    TEST_ASSERT_EQUAL_UINT(1, gOurSecs.size());
    TEST_ASSERT_EQUAL_INT(-1, enqueueCurrentBleBondBackupSnapshot());
    storage.setFilesystem(&gFs, true);
    fs::mock_fail_next_read_open(kTestBondPath);
    restoreOrBackupBleBondsAtBoot(storage, gOurSecs.size());
    TEST_ASSERT_TRUE(bleBondRecoveryPending());
    TEST_ASSERT_EQUAL_UINT(0, gBondWriteCalls);
    bootRecoveryOwner();
    expectCompleteRecoveredBackup();
}

void test_normal_boot_without_sd_retains_existing_bonds_and_allows_runtime_changes() {
    gOurSecs = {makeSec(1)};
    storage.reset();
    initializeBleBondRecovery();
    restoreOrBackupBleBondsAtBoot(storage, 1);
    TEST_ASSERT_FALSE(bleBondRecoveryPending());
    TEST_ASSERT_EQUAL_UINT(1, gOurSecs.size());
    TEST_ASSERT_EQUAL_UINT(0, gBondWriteCalls);
    TEST_ASSERT_EQUAL_UINT(1, bleBondBackupQueueDepthForTest());
}

void test_pending_restore_from_previous_backup_survives_failed_peer_write() {
    seedCompleteRecovery(true);
    gFailBondWriteCall = 3;
    restoreOrBackupBleBondsAtBoot(storage, 0);
    TEST_ASSERT_TRUE(bleBondRecoveryPending());
    TEST_ASSERT_TRUE(gFs.exists(kTestBondPrevious));
    bootRecoveryOwner();
    expectCompleteRecoveredBackup();
}

void test_pending_restore_preserves_newer_pairing_keys_and_new_bonds() {
    seedCompleteRecovery();
    gFailBondWriteCall = 2;
    restoreOrBackupBleBondsAtBoot(storage, 0);
    gDurableOurSecs[0].payload[0] = 99;
    gDurableOurSecs.push_back(makeSec(31));
    bootRecoveryOwner();
    TEST_ASSERT_FALSE(bleBondRecoveryPending());
    TEST_ASSERT_EQUAL_UINT(3, gDurableOurSecs.size());
    TEST_ASSERT_EQUAL_UINT8(99, gOurSecs[0].payload[0]);
    TEST_ASSERT_EQUAL_UINT8(99, gDurableOurSecs[0].payload[0]);
    bootRecoveryOwner();
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT32(3, readHeader().ourSecCount);
}

void test_intentional_deletion_after_completed_restore_is_not_revived() {
    seedCompleteRecovery();
    bootRecoveryOwner();
    TEST_ASSERT_FALSE(bleBondRecoveryPending());
    // Successful normal deletion persists in NimBLE before the regular backup.
    gDurableOurSecs.clear();
    gDurablePeerSecs.clear();
    gOurSecs.clear();
    gPeerSecs.clear();
    TEST_ASSERT_EQUAL_INT(0, enqueueCurrentBleBondBackupSnapshot());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    bootRecoveryOwner();
    TEST_ASSERT_FALSE(bleBondRecoveryPending());
    TEST_ASSERT_TRUE(gOurSecs.empty());
    TEST_ASSERT_TRUE(gPeerSecs.empty());
    TEST_ASSERT_EQUAL_UINT(0, gBondWriteCalls);
}

void test_recovery_at_capacity_keeps_all_durable_bonds_and_complete_backup() {
    seedCompleteRecovery();
    gFailBondWriteCall = 2;
    restoreOrBackupBleBondsAtBoot(storage, 0);
    gDurableOurSecs.push_back(makeSec(31));
    gBondCapacity = 2;
    bootRecoveryOwner();
    TEST_ASSERT_TRUE(bleBondRecoveryPending());
    TEST_ASSERT_EQUAL_UINT(0, gOldestBondEvictions);
    TEST_ASSERT_EQUAL_UINT(2, gDurableOurSecs.size());
    TEST_ASSERT_EQUAL_UINT8(1, gOurSecs[0].peer_addr[0]);
    TEST_ASSERT_EQUAL_UINT8(31, gOurSecs[1].peer_addr[0]);
    bootRecoveryOwner();
    TEST_ASSERT_TRUE(bleBondRecoveryPending());
    TEST_ASSERT_EQUAL_UINT(0, gOldestBondEvictions);
    TEST_ASSERT_EQUAL_UINT(2, gOurSecs.size());
    TEST_ASSERT_EQUAL_UINT8(1, gOurSecs[0].peer_addr[0]);
    TEST_ASSERT_EQUAL_UINT8(31, gOurSecs[1].peer_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(2, readHeader().ourSecCount);
    TEST_ASSERT_EQUAL_UINT32(1, readHeader().peerSecCount);
}

void test_normal_store_overflow_retains_nimble_default_handling() {
    TEST_ASSERT_FALSE(bleBondRecoveryPending());
    TEST_ASSERT_EQUAL_INT(0, handleBleBondStoreStatus(nullptr, nullptr));
    TEST_ASSERT_EQUAL_UINT(1, gOldestBondEvictions);
}

void test_boot_recovery_precedes_advertising_and_gates_schema_and_runtime_deletion() {
    const std::string client = readProjectFile("src/ble_client.cpp");
    const std::string init = functionBody(client, "bool V1BLEClient::initBLE(");
    TEST_ASSERT_TRUE(init.find("initializeBleBondRecovery()") < init.find("NimBLEDevice::init("));
    TEST_ASSERT_TRUE(init.find("setDeviceCallbacks(&bondRecoveryCallbacks)") < init.find("restoreOrBackupBleBondsAtBoot"));
    TEST_ASSERT_TRUE(init.find("restoreOrBackupBleBondsAtBoot") < init.find("initProxyServer("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, init.find("needsBondSchemaMigration && !bleBondRecoveryPending()"));
    const auto runtime = functionBody(readProjectFile("src/ble_runtime.cpp"), "void V1BLEClient::applyDeferredRuntimeEvents(");
    TEST_ASSERT_TRUE(runtime.find("pendingDeleteBond_ && !bleBondRecoveryPending()") < runtime.find("pendingDeleteBond_ = false"));
}

void test_queued_pre_recovery_snapshot_is_discarded_after_successful_restore() {
    seedCompleteRecovery();
    // Models the still-queued empty snapshot after migration's bounded wait
    // expires; recovery obtains the SD lock before the writer drains it.
    TEST_ASSERT_EQUAL_INT(0, enqueueCurrentBleBondBackupSnapshot());
    const uint32_t staleSequence = gBondBackupWriterState.nextSequence.load();
    restoreOrBackupBleBondsAtBoot(storage, 0);
    TEST_ASSERT_FALSE(bleBondRecoveryPending());
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT(0, bleBondBackupQueueDepthForTest());
    TEST_ASSERT_NOT_EQUAL(staleSequence, gBondBackupWriterState.lastSuccessfulSequence.load());
    TEST_ASSERT_EQUAL_UINT32(2, readHeader().ourSecCount);
    TEST_ASSERT_EQUAL_UINT32(1, readHeader().peerSecCount);
    expectCompleteRecoveredBackup();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_queued_pre_recovery_snapshot_is_discarded_after_successful_restore);
    RUN_TEST(test_recovery_at_capacity_keeps_all_durable_bonds_and_complete_backup);
    RUN_TEST(test_normal_store_overflow_retains_nimble_default_handling);
    RUN_TEST(test_boot_recovery_precedes_advertising_and_gates_schema_and_runtime_deletion);
    RUN_TEST(test_interrupted_restore_resumes_after_each_durable_record_without_replacing_backup);
    RUN_TEST(test_failed_our_or_peer_write_keeps_recovery_pending_until_fresh_boot);
    RUN_TEST(test_recovery_marker_failure_does_not_start_nimble_writes);
    RUN_TEST(test_reset_after_recovery_marker_before_first_bond_write_resumes);
    RUN_TEST(test_recovery_marker_read_or_type_error_defers_recovery_and_snapshots);
    RUN_TEST(test_recovery_marker_verification_failure_does_not_start_nimble_writes);
    RUN_TEST(test_recovery_marker_clear_failure_keeps_complete_source_until_next_boot);
    RUN_TEST(test_pending_restore_waits_for_absent_or_unreadable_sd_without_losing_bonds);
    RUN_TEST(test_normal_boot_without_sd_retains_existing_bonds_and_allows_runtime_changes);
    RUN_TEST(test_pending_restore_from_previous_backup_survives_failed_peer_write);
    RUN_TEST(test_pending_restore_preserves_newer_pairing_keys_and_new_bonds);
    RUN_TEST(test_intentional_deletion_after_completed_restore_is_not_revived);
    RUN_TEST(test_restore_open_failure_never_seeds_previous_bonds_into_nvs);
    RUN_TEST(test_restore_short_header_or_our_entry_read_never_seeds_previous_bonds_into_nvs);
    RUN_TEST(test_restore_short_peer_entry_read_writes_no_partially_read_bonds);
    RUN_TEST(test_restore_unavailable_live_metadata_does_not_select_readable_previous);
    RUN_TEST(test_restore_readable_live_remains_latest_after_normal_boot_backup);
    RUN_TEST(test_restore_missing_live_still_uses_previous_through_normal_boot_backup);
    RUN_TEST(test_restore_corrupt_live_still_uses_previous_through_normal_boot_backup);
    RUN_TEST(test_restore_valid_empty_live_does_not_resurrect_previous_bonds);
    RUN_TEST(test_unavailable_cached_file_size_cannot_demote_the_latest_bond_backup);
    RUN_TEST(test_corrupt_live_failed_bond_promotion_preserves_previous_through_reboot_and_retry);
    RUN_TEST(test_missing_live_failed_bond_promotion_preserves_previous_through_reboot_and_retry);
    RUN_TEST(test_healthy_live_failed_bond_promotion_restores_latest_through_reboot_and_retry);
    RUN_TEST(test_failed_bond_promotion_and_rollback_preserve_latest_through_reboot_and_retry);
    RUN_TEST(test_reset_after_corrupt_bond_live_removal_preserves_previous);
    RUN_TEST(test_reset_after_valid_bond_live_rotation_preserves_latest);
    RUN_TEST(test_reset_after_recovered_bond_promotion_selects_new_commit);
    RUN_TEST(test_reset_after_healthy_bond_promotion_selects_new_commit);
    RUN_TEST(test_bond_live_remove_failure_preserves_both_recovery_candidates);
    RUN_TEST(test_unreadable_live_bond_backup_does_not_rotate_or_overwrite_recovery);
    RUN_TEST(test_unreadable_previous_bond_backup_does_not_replace_it_with_corrupt_live);
    RUN_TEST(test_bond_candidate_validation_failure_preserves_committed_backup_and_retries);
    RUN_TEST(test_bond_validation_never_changes_the_live_nimble_store);
    RUN_TEST(test_runtime_enqueue_is_nonblocking_and_writer_is_pinned_to_core_zero);
    RUN_TEST(test_zero_bond_snapshot_replaces_stale_backup_after_auto_heal);
    RUN_TEST(test_full_queue_coalesces_to_latest_snapshot_without_waiting);
    RUN_TEST(test_writer_failure_retains_snapshot_for_retry);
    RUN_TEST(test_writer_setup_failure_releases_snapshot_without_sd_access);
    RUN_TEST(test_aborted_shutdown_reopens_bond_backup_writer_admission);
    RUN_TEST(test_resume_handoffs_snapshot_queued_while_old_bond_writer_exits);
    RUN_TEST(test_shutdown_exit_drains_late_bond_snapshot_and_waits_for_admissions);
    RUN_TEST(test_runtime_callers_contain_no_sd_lock_or_filesystem_write);
    return UNITY_END();
}
