#include <unity.h>

#include <fstream>
#include <string>

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

void setUp() {}
void tearDown() {}

void test_audio_process_amp_timeout_uses_nonblocking_disable_and_only_clears_on_success() {
    const std::string source = readFile((projectRoot() + "/src/audio_voice.cpp").c_str());
    const std::string body = extractBlock(source, "void audio_process_amp_timeout()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("AMP_TIMEOUT_CHECK_INTERVAL_MS"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("lastAmpTimeoutCheckMs"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("set_speaker_amp(false, 0)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("if (result == AudioI2cResult::Ok)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("amp_is_warm = false;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("else if (result != AudioI2cResult::Busy)"));
}

void test_audio_tasks_abort_when_codec_init_or_amp_enable_fails() {
    const std::string beepSource = readFile((projectRoot() + "/src/audio_beep.cpp").c_str());
    const std::string beepTask = extractBlock(beepSource, "static void audio_playback_task(void* pvParameters)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, beepTask.find("if (!es8311_init())"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, beepTask.find("if (ampEnableResult != AudioI2cResult::Ok)"));

    const std::string voiceSource = readFile((projectRoot() + "/src/audio_voice.cpp").c_str());
    const std::string voiceTask = extractBlock(voiceSource, "static void sd_audio_playback_task(void* pvParameters) {");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, voiceTask.find("if (!es8311_init())"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, voiceTask.find("if (ampEnableResult != AudioI2cResult::Ok)"));
}

void test_battery_critical_poweroff_checks_latch_drop_failure_with_extended_budget() {
    const std::string source = readFile((projectRoot() + "/src/battery_manager.cpp").c_str());
    const std::string powerOffBody = extractBlock(source, "bool BatteryManager::powerOff(bool sdLogEnabled)");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          powerOffBody.find("setTCA9554PinWithBudget(TCA9554_PWR_LATCH_PIN"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, powerOffBody.find("pdMS_TO_TICKS(250)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, powerOffBody.find("5)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, powerOffBody.find("if (latchDropped)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          powerOffBody.find("readTca9554RegisterWithTimeout("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          powerOffBody.find("Failed to drop power latch, falling back to deep sleep"));
}

void test_battery_shutdown_readback_uses_explicit_wire_timeout_and_restores_it() {
    const std::string source = readFile((projectRoot() + "/src/battery_manager.cpp").c_str());
    const std::string helperBody = extractBlock(source, "AudioI2cResult readTca9554RegisterWithTimeout(");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("kShutdownReadbackTimeoutMs = 50"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, helperBody.find("AudioI2cLockGuard lock"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, helperBody.find("ScopedWireTimeout timeoutGuard"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          helperBody.find("audioI2cReadRegister(tca9554Wire, TCA9554_I2C_ADDR, reg, value)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("wire.getTimeOut()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("wire_.setTimeOut(timeoutMs)"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_audio_process_amp_timeout_uses_nonblocking_disable_and_only_clears_on_success);
    RUN_TEST(test_audio_tasks_abort_when_codec_init_or_amp_enable_fails);
    RUN_TEST(test_battery_critical_poweroff_checks_latch_drop_failure_with_extended_budget);
    RUN_TEST(test_battery_shutdown_readback_uses_explicit_wire_timeout_and_restores_it);
    return UNITY_END();
}
