#include <unity.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/ble_bond_backup_store.h"
#include "../../src/ble_bond_backup_store.cpp"

namespace {

std::filesystem::path makeFsRoot(const char* testName) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("v1simple_ble_bond_restore_" + std::string(testName));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

std::filesystem::path resolveFsPath(const std::filesystem::path& root, const char* logicalPath) {
    std::filesystem::path relative = logicalPath ? std::filesystem::path(logicalPath) : std::filesystem::path();
    if (relative.is_absolute()) {
        relative = relative.relative_path();
    }
    return root / relative;
}

std::vector<ble_store_value_sec> g_restoredOurSecs;
std::vector<ble_store_value_sec> g_restoredPeerSecs;

void writeBondBackup(const std::filesystem::path& path,
                     const std::vector<ble_store_value_sec>& ourSecs,
                     const std::vector<ble_store_value_sec>& peerSecs) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    TEST_ASSERT_TRUE(stream.is_open());

    BondBackupHeader header = {};
    memcpy(header.magic, kBleBondMagic, sizeof(header.magic));
    header.ourSecCount = static_cast<uint32_t>(ourSecs.size());
    header.peerSecCount = static_cast<uint32_t>(peerSecs.size());
    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!ourSecs.empty()) {
        stream.write(reinterpret_cast<const char*>(ourSecs.data()),
                     static_cast<std::streamsize>(ourSecs.size() * sizeof(ble_store_value_sec)));
    }
    if (!peerSecs.empty()) {
        stream.write(reinterpret_cast<const char*>(peerSecs.data()),
                     static_cast<std::streamsize>(peerSecs.size() * sizeof(ble_store_value_sec)));
    }
    TEST_ASSERT_TRUE(stream.good());
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

}  // namespace

extern "C" int ble_store_iterate(int, ble_store_iterator_fn, void*) {
    return 0;
}

extern "C" int ble_store_write_our_sec(const ble_store_value_sec* value) {
    g_restoredOurSecs.push_back(*value);
    return 0;
}

extern "C" int ble_store_write_peer_sec(const ble_store_value_sec* value) {
    g_restoredPeerSecs.push_back(*value);
    return 0;
}

void setUp() {
    g_restoredOurSecs.clear();
    g_restoredPeerSecs.clear();
}

void tearDown() {}

void test_bond_restore_prefers_valid_live_over_prev() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS fs(root);

    writeBondBackup(resolveFsPath(root, "/v1simple_ble_bonds.bin"),
                    {makeSec(1)},
                    {makeSec(11)});
    writeBondBackup(resolveFsPath(root, "/v1simple_ble_bonds.bin.prev"),
                    {makeSec(21)},
                    {makeSec(31)});

    TEST_ASSERT_EQUAL(2, restoreBleBondBackup(fs, "/v1simple_ble_bonds.bin"));
    TEST_ASSERT_EQUAL_UINT32(1, g_restoredOurSecs.size());
    TEST_ASSERT_EQUAL_UINT32(1, g_restoredPeerSecs.size());
    TEST_ASSERT_EQUAL_UINT8(1, g_restoredOurSecs[0].peer_addr[0]);
    TEST_ASSERT_EQUAL_UINT8(11, g_restoredPeerSecs[0].peer_addr[0]);
}

void test_bond_restore_falls_back_to_prev_when_live_missing() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS fs(root);

    writeBondBackup(resolveFsPath(root, "/v1simple_ble_bonds.bin.prev"),
                    {makeSec(21)},
                    {makeSec(31)});

    TEST_ASSERT_EQUAL(2, restoreBleBondBackup(fs, "/v1simple_ble_bonds.bin"));
    TEST_ASSERT_EQUAL_UINT32(1, g_restoredOurSecs.size());
    TEST_ASSERT_EQUAL_UINT32(1, g_restoredPeerSecs.size());
    TEST_ASSERT_EQUAL_UINT8(21, g_restoredOurSecs[0].peer_addr[0]);
    TEST_ASSERT_EQUAL_UINT8(31, g_restoredPeerSecs[0].peer_addr[0]);
}

void test_bond_restore_falls_back_to_prev_when_live_is_corrupt() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS fs(root);

    std::ofstream live(resolveFsPath(root, "/v1simple_ble_bonds.bin"), std::ios::binary | std::ios::trunc);
    TEST_ASSERT_TRUE(live.is_open());
    live << "bad";
    live.close();

    writeBondBackup(resolveFsPath(root, "/v1simple_ble_bonds.bin.prev"),
                    {makeSec(21)},
                    {makeSec(31)});

    TEST_ASSERT_EQUAL(2, restoreBleBondBackup(fs, "/v1simple_ble_bonds.bin"));
    TEST_ASSERT_EQUAL_UINT32(1, g_restoredOurSecs.size());
    TEST_ASSERT_EQUAL_UINT32(1, g_restoredPeerSecs.size());
    TEST_ASSERT_EQUAL_UINT8(21, g_restoredOurSecs[0].peer_addr[0]);
    TEST_ASSERT_EQUAL_UINT8(31, g_restoredPeerSecs[0].peer_addr[0]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_bond_restore_prefers_valid_live_over_prev);
    RUN_TEST(test_bond_restore_falls_back_to_prev_when_live_missing);
    RUN_TEST(test_bond_restore_falls_back_to_prev_when_live_is_corrupt);
    return UNITY_END();
}
