#include <unity.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../mocks/Arduino.h"
#include "../mocks/FS.h"
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

extern "C" int ble_store_write_our_sec(const ble_store_value_sec*) {
    return 0;
}

extern "C" int ble_store_write_peer_sec(const ble_store_value_sec*) {
    return 0;
}

#include "../../src/psram_freertos_alloc.cpp"
#include "../../src/ble_bond_backup_writer.cpp"

void setUp() {
    gRoot = std::filesystem::temp_directory_path() / "v1simple_ble_bond_writer";
    std::error_code ec;
    std::filesystem::remove_all(gRoot, ec);
    std::filesystem::create_directories(gRoot, ec);
    gFs = fs::FS(gRoot);
    gOurSecs.clear();
    gPeerSecs.clear();
    storageManager.reset();
    storageManager.setFilesystem(&gFs, true);
    StorageManager::resetMockSdLockState();
    mock_reset_heap_caps();
    mock_reset_queue_create_state();
    mock_reset_task_create_state();
    resetBleBondBackupWriterForTest();
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

    const BleBondBackupWriterStats stats = bleBondBackupWriterStats();
    TEST_ASSERT_EQUAL_UINT32(1, stats.enqueuedSnapshots);
    TEST_ASSERT_EQUAL_UINT32(1, stats.successfulWrites);
    TEST_ASSERT_EQUAL_UINT32(0, stats.writeFailures);
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

    const BleBondBackupWriterStats stats = bleBondBackupWriterStats();
    TEST_ASSERT_EQUAL_UINT32(2, stats.enqueuedSnapshots);
    TEST_ASSERT_EQUAL_UINT32(1, stats.coalescedSnapshots);
    TEST_ASSERT_EQUAL_UINT32(0, stats.droppedSnapshots);
}

void test_writer_failure_retains_snapshot_for_retry() {
    gOurSecs = {makeSec(44)};
    TEST_ASSERT_EQUAL(1, enqueueCurrentBleBondBackupSnapshot());

    storageManager.reset();
    TEST_ASSERT_FALSE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT(1, bleBondBackupQueueDepthForTest());

    storageManager.setFilesystem(&gFs, true);
    TEST_ASSERT_TRUE(runBleBondBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT8(44, readFirstOurAddressByte());

    const BleBondBackupWriterStats stats = bleBondBackupWriterStats();
    TEST_ASSERT_EQUAL_UINT32(1, stats.writeFailures);
    TEST_ASSERT_EQUAL_UINT32(1, stats.retryRequeues);
    TEST_ASSERT_EQUAL_UINT32(1, stats.successfulWrites);
}

void test_writer_setup_failure_drops_observably_without_sd_access() {
    gOurSecs = {makeSec(55)};
    g_mock_task_create_state.failCaps = true;

    TEST_ASSERT_EQUAL(-1, enqueueCurrentBleBondBackupSnapshot());

    TEST_ASSERT_EQUAL_UINT(0, bleBondBackupQueueDepthForTest());
    TEST_ASSERT_EQUAL_UINT32(0, StorageManager::mockSdLockState.blockingAcquireCalls);
    TEST_ASSERT_EQUAL_UINT32(1, bleBondBackupWriterStats().droppedSnapshots);
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

void test_writer_stack_minimum_preserves_zero_and_reset_restores_unsampled() {
    TEST_ASSERT_EQUAL_UINT32(
        UINT32_MAX, bleBondBackupWriterStats().writerStackMinFreeBytes);

    recordBleBondBackupWriterStackSampleForTest(768);
    recordBleBondBackupWriterStackSampleForTest(1024);
    TEST_ASSERT_EQUAL_UINT32(
        768, bleBondBackupWriterStats().writerStackMinFreeBytes);

    recordBleBondBackupWriterStackSampleForTest(0);
    recordBleBondBackupWriterStackSampleForTest(512);
    TEST_ASSERT_EQUAL_UINT32(
        0, bleBondBackupWriterStats().writerStackMinFreeBytes);

    resetBleBondBackupWriterForTest();
    TEST_ASSERT_EQUAL_UINT32(
        UINT32_MAX, bleBondBackupWriterStats().writerStackMinFreeBytes);
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
        writer, "void bondBackupWriterTaskEntry(void*)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          writerTask.find("sampleBondBackupWriterStack()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          writerTask.find("vTaskDeleteWithCaps(nullptr)"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_runtime_enqueue_is_nonblocking_and_writer_is_pinned_to_core_zero);
    RUN_TEST(test_zero_bond_snapshot_replaces_stale_backup_after_auto_heal);
    RUN_TEST(test_full_queue_coalesces_to_latest_snapshot_without_waiting);
    RUN_TEST(test_writer_failure_retains_snapshot_for_retry);
    RUN_TEST(test_writer_setup_failure_drops_observably_without_sd_access);
    RUN_TEST(test_aborted_shutdown_reopens_bond_backup_writer_admission);
    RUN_TEST(test_resume_handoffs_snapshot_queued_while_old_bond_writer_exits);
    RUN_TEST(test_shutdown_exit_drains_late_bond_snapshot_and_waits_for_admissions);
    RUN_TEST(test_writer_stack_minimum_preserves_zero_and_reset_restores_unsampled);
    RUN_TEST(test_runtime_callers_contain_no_sd_lock_or_filesystem_write);
    return UNITY_END();
}
