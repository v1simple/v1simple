#include <unity.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    TEST_ASSERT_TRUE_MESSAGE(input.is_open(), path.string().c_str());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::string extractFunction(const std::string& source, const std::string& signature) {
    const size_t signaturePos = source.find(signature);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, signaturePos, signature.c_str());
    const size_t open = source.find('{', signaturePos);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, open, signature.c_str());

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

void assertUsesCapsDelete(const char* relativePath, const char* signature) {
    const std::filesystem::path path =
        std::filesystem::path(PROJECT_DIR) / relativePath;
    const std::string body = extractFunction(readFile(path), signature);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        std::string::npos, body.find("vTaskDeleteWithCaps(nullptr)"), relativePath);
    TEST_ASSERT_EQUAL_MESSAGE(
        std::string::npos, body.find("vTaskDelete(nullptr)"), relativePath);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_helper_created_task_exit_paths_use_matching_caps_delete() {
    assertUsesCapsDelete("src/settings_backup.cpp",
                         "void deferredBackupWriterTaskEntry(void*)");
    assertUsesCapsDelete("src/modules/alp/alp_sd_logger.cpp",
                         "void AlpSdLogger::writerTaskLoop()");
    assertUsesCapsDelete("src/modules/obd/obd_runtime_transport.cpp",
                         "void obdTransportTaskEntry(void* param)");
    assertUsesCapsDelete("src/ble_bond_backup_writer.cpp",
                         "void bondBackupWriterTaskEntry(void*)");
}

void test_caps_task_creation_translation_units_have_no_standard_self_delete() {
    const std::filesystem::path srcRoot =
        std::filesystem::path(PROJECT_DIR) / "src";
    size_t capsCreationFiles = 0;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(srcRoot)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
            continue;
        }

        const std::string source = readFile(entry.path());
        const bool createsCapsTask =
            source.find("createTaskPinnedToCoreInternalStack(") != std::string::npos ||
            source.find("xTaskCreatePinnedToCoreWithCaps(") != std::string::npos;
        if (!createsCapsTask) {
            continue;
        }

        ++capsCreationFiles;
        TEST_ASSERT_EQUAL_MESSAGE(
            std::string::npos,
            source.find("vTaskDelete(nullptr)"),
            entry.path().string().c_str());
    }

    TEST_ASSERT_GREATER_OR_EQUAL_UINT(8, capsCreationFiles);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_helper_created_task_exit_paths_use_matching_caps_delete);
    RUN_TEST(test_caps_task_creation_translation_units_have_no_standard_self_delete);
    return UNITY_END();
}
