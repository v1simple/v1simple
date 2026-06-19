#include <unity.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <ArduinoJson.h>
#include "../mocks/Arduino.h"
#include "../mocks/mock_heap_caps_state.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/littlefs_mount.cpp"
#include "../../src/storage_manager.cpp"

namespace {

std::filesystem::path makeFsRoot(const char* testName) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("v1simple_storage_manager_" + std::string(testName));
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

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    TEST_ASSERT_TRUE(stream.is_open());
    stream << contents;
    TEST_ASSERT_TRUE(stream.good());
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    TEST_ASSERT_TRUE(stream.is_open());
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

void setUp() {
    mock_reset_heap_caps();
    fs::mock_reset_fs_rename_state();
    fs::mock_reset_fs_write_budget();
}

void tearDown() {}

void test_promote_temp_file_succeeds_when_no_live_file_exists() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS testFs(root);
    writeFile(resolveFsPath(root, "/settings.json.tmp"), "new");

    TEST_ASSERT_TRUE(
        StorageManager::promoteTempFileWithRollback(testFs, "/settings.json.tmp", "/settings.json"));

    TEST_ASSERT_TRUE(testFs.exists("/settings.json"));
    TEST_ASSERT_FALSE(testFs.exists("/settings.json.tmp"));
    TEST_ASSERT_FALSE(testFs.exists("/settings.json.prev"));
    TEST_ASSERT_EQUAL_STRING("new", readFile(resolveFsPath(root, "/settings.json")).c_str());
}

void test_promote_temp_file_replaces_existing_live_file_and_cleans_backup() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS testFs(root);
    writeFile(resolveFsPath(root, "/settings.json"), "old");
    writeFile(resolveFsPath(root, "/settings.json.tmp"), "new");

    TEST_ASSERT_TRUE(
        StorageManager::promoteTempFileWithRollback(testFs, "/settings.json.tmp", "/settings.json"));

    TEST_ASSERT_TRUE(testFs.exists("/settings.json"));
    TEST_ASSERT_FALSE(testFs.exists("/settings.json.prev"));
    TEST_ASSERT_EQUAL_STRING("new", readFile(resolveFsPath(root, "/settings.json")).c_str());
}

void test_promote_temp_file_fails_when_live_file_cannot_rotate() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS testFs(root);
    writeFile(resolveFsPath(root, "/settings.json"), "old");
    writeFile(resolveFsPath(root, "/settings.json.tmp"), "new");
    fs::mock_fail_next_rename();

    TEST_ASSERT_FALSE(
        StorageManager::promoteTempFileWithRollback(testFs, "/settings.json.tmp", "/settings.json"));

    TEST_ASSERT_TRUE(testFs.exists("/settings.json"));
    TEST_ASSERT_FALSE(testFs.exists("/settings.json.prev"));
    TEST_ASSERT_FALSE(testFs.exists("/settings.json.tmp"));
    TEST_ASSERT_EQUAL_STRING("old", readFile(resolveFsPath(root, "/settings.json")).c_str());
}

void test_promote_temp_file_rolls_back_when_temp_promotion_fails() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS testFs(root);
    writeFile(resolveFsPath(root, "/settings.json"), "old");
    writeFile(resolveFsPath(root, "/settings.json.tmp"), "new");
    fs::mock_fail_rename_on_call(2);

    TEST_ASSERT_FALSE(
        StorageManager::promoteTempFileWithRollback(testFs, "/settings.json.tmp", "/settings.json"));

    TEST_ASSERT_TRUE(testFs.exists("/settings.json"));
    TEST_ASSERT_FALSE(testFs.exists("/settings.json.prev"));
    TEST_ASSERT_FALSE(testFs.exists("/settings.json.tmp"));
    TEST_ASSERT_EQUAL_STRING("old", readFile(resolveFsPath(root, "/settings.json")).c_str());
}

void test_promote_temp_file_keeps_prev_when_live_missing_and_promotion_fails() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS testFs(root);
    writeFile(resolveFsPath(root, "/settings.json.prev"), "old");
    writeFile(resolveFsPath(root, "/settings.json.tmp"), "new");
    fs::mock_fail_next_rename();

    TEST_ASSERT_FALSE(
        StorageManager::promoteTempFileWithRollback(testFs, "/settings.json.tmp", "/settings.json"));

    TEST_ASSERT_FALSE(testFs.exists("/settings.json"));
    TEST_ASSERT_FALSE(testFs.exists("/settings.json.tmp"));
    TEST_ASSERT_TRUE(testFs.exists("/settings.json.prev"));
    TEST_ASSERT_EQUAL_STRING("old", readFile(resolveFsPath(root, "/settings.json.prev")).c_str());
}

void test_promote_temp_file_cleans_prev_when_live_missing_and_promotion_succeeds() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS testFs(root);
    writeFile(resolveFsPath(root, "/settings.json.prev"), "old");
    writeFile(resolveFsPath(root, "/settings.json.tmp"), "new");

    TEST_ASSERT_TRUE(
        StorageManager::promoteTempFileWithRollback(testFs, "/settings.json.tmp", "/settings.json"));

    TEST_ASSERT_TRUE(testFs.exists("/settings.json"));
    TEST_ASSERT_FALSE(testFs.exists("/settings.json.tmp"));
    TEST_ASSERT_FALSE(testFs.exists("/settings.json.prev"));
    TEST_ASSERT_EQUAL_STRING("new", readFile(resolveFsPath(root, "/settings.json")).c_str());
}

// --- writeJsonFileAtomic tests ---

void test_write_json_file_atomic_creates_file_with_correct_content() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS testFs(root);
    JsonDocument doc;
    doc["key"] = "value";
    doc["num"] = 42;

    TEST_ASSERT_TRUE(StorageManager::writeJsonFileAtomic(testFs, "/out.json", doc));

    TEST_ASSERT_TRUE(testFs.exists("/out.json"));
    TEST_ASSERT_FALSE(testFs.exists("/out.json.tmp"));
    const std::string content = readFile(resolveFsPath(root, "/out.json"));
    TEST_ASSERT_TRUE(content.find("\"key\"") != std::string::npos);
    TEST_ASSERT_TRUE(content.find("\"value\"") != std::string::npos);
    TEST_ASSERT_TRUE(content.find("42") != std::string::npos);
}

void test_write_json_file_atomic_returns_false_and_removes_tmp_when_write_budget_zero() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS testFs(root);
    fs::mock_set_fs_write_budget(0);
    JsonDocument doc;
    doc["key"] = "value";

    TEST_ASSERT_FALSE(StorageManager::writeJsonFileAtomic(testFs, "/out.json", doc));

    TEST_ASSERT_FALSE(testFs.exists("/out.json"));
    TEST_ASSERT_FALSE(testFs.exists("/out.json.tmp"));
}

void test_write_json_file_atomic_returns_false_when_promote_fails() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS testFs(root);
    // First rename call in promoteTempFileWithRollback tries to move live→prev (no live file here),
    // so the first rename is the temp→live promotion; fail it.
    fs::mock_fail_next_rename();
    JsonDocument doc;
    doc["x"] = 1;

    TEST_ASSERT_FALSE(StorageManager::writeJsonFileAtomic(testFs, "/out.json", doc));

    TEST_ASSERT_FALSE(testFs.exists("/out.json"));
}

void test_write_json_file_atomic_null_path_returns_false() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS testFs(root);
    JsonDocument doc;
    doc["x"] = 1;

    // null path should not crash and should return false
    TEST_ASSERT_FALSE(StorageManager::writeJsonFileAtomic(testFs, nullptr, doc));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_promote_temp_file_succeeds_when_no_live_file_exists);
    RUN_TEST(test_promote_temp_file_replaces_existing_live_file_and_cleans_backup);
    RUN_TEST(test_promote_temp_file_fails_when_live_file_cannot_rotate);
    RUN_TEST(test_promote_temp_file_rolls_back_when_temp_promotion_fails);
    RUN_TEST(test_promote_temp_file_keeps_prev_when_live_missing_and_promotion_fails);
    RUN_TEST(test_promote_temp_file_cleans_prev_when_live_missing_and_promotion_succeeds);
    RUN_TEST(test_write_json_file_atomic_creates_file_with_correct_content);
    RUN_TEST(test_write_json_file_atomic_returns_false_and_removes_tmp_when_write_budget_zero);
    RUN_TEST(test_write_json_file_atomic_returns_false_when_promote_fails);
    RUN_TEST(test_write_json_file_atomic_null_path_returns_false);
    return UNITY_END();
}
