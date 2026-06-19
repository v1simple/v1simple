#include <unity.h>

#include <fstream>
#include <string>

#include "../mocks/freertos/FreeRTOS.h"
#include "../mocks/freertos/task.h"

namespace {

static std::string projectRoot() {
    return std::string(PROJECT_DIR);
}

std::string readFile(const char* path) {
    std::ifstream input(path);
    TEST_ASSERT_TRUE_MESSAGE(input.good(), "failed to open source file");
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

size_t findMatchingBrace(const std::string& text, size_t openPos) {
    TEST_ASSERT_NOT_EQUAL(std::string::npos, openPos);
    int depth = 0;
    for (size_t i = openPos; i < text.size(); ++i) {
        if (text[i] == '{') {
            depth++;
        } else if (text[i] == '}') {
            depth--;
            if (depth == 0) {
                return i;
            }
        }
    }
    return std::string::npos;
}

std::string extractBlock(const std::string& text, const std::string& marker) {
    const size_t markerPos = text.find(marker);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, markerPos);
    const size_t openBrace = text.find('{', markerPos);
    const size_t closeBrace = findMatchingBrace(text, openBrace);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, closeBrace);
    return text.substr(openBrace, closeBrace - openBrace + 1);
}

}  // namespace

void setUp() {
    mock_reset_task_delete_state();
}

void tearDown() {}

void test_delete_mocks_record_standard_and_caps_calls() {
    vTaskDelete(nullptr);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_task_delete_state.standardCalls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_task_delete_state.capsCalls);

    mock_reset_task_delete_state();
    vTaskDeleteWithCaps(nullptr);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_task_delete_state.standardCalls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_task_delete_state.capsCalls);
}

void test_i2s_init_failure_path_uses_caps_delete() {
    const std::string source = readFile((projectRoot() + "/src/audio_beep.cpp").c_str());
    const std::string taskBody = extractBlock(source, "static void audio_playback_task(void* pvParameters)");
    const std::string failureBlock = extractBlock(taskBody, "if (!i2s_initialized)");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, failureBlock.find("audioResetTaskState(audio_playing, audioTaskHandle);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, failureBlock.find("vTaskDeleteWithCaps(nullptr);"));
    TEST_ASSERT_EQUAL(std::string::npos, failureBlock.find("vTaskDelete(nullptr);"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_delete_mocks_record_standard_and_caps_calls);
    RUN_TEST(test_i2s_init_failure_path_uses_caps_delete);
    return UNITY_END();
}
