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

size_t countOccurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        count++;
        position += needle.size();
    }
    return count;
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

void test_deep_sleep_wake_releases_hold_with_backlight_forced_off() {
    const std::string source = readFile((projectRoot() + "/src/main_setup_helpers.cpp").c_str());
    const std::string body = extractBlock(source, "void initializeEarlyBootDiagnostics()");

    const size_t preload = body.find("digitalWrite(LCD_BL, HIGH)");
    const size_t releaseGlobal = body.find("gpio_deep_sleep_hold_dis()");
    const size_t releasePin = body.find("gpio_hold_dis(static_cast<gpio_num_t>(LCD_BL))");
    const size_t assertAfterRelease = body.find("digitalWrite(LCD_BL, HIGH)", preload + 1);

    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("pinMode(LCD_BL, OUTPUT)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, preload);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, releaseGlobal);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, releasePin);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, assertAfterRelease);
    TEST_ASSERT_TRUE(preload < releaseGlobal);
    TEST_ASSERT_TRUE(releaseGlobal < releasePin);
    TEST_ASSERT_TRUE(releasePin < assertAfterRelease);
}

void test_poweroff_outcome_is_terminal_deep_sleep_evidence() {
    const std::string source = readFile((projectRoot() + "/src/battery_manager.cpp").c_str());
    const std::string deepSleepBody = extractBlock(source, "bool BatteryManager::enterDeepSleep(");
    const std::string powerOffBody = extractBlock(source, "bool BatteryManager::powerOff(bool sdLogEnabled)");

    const size_t wakeConfig = deepSleepBody.find("esp_sleep_enable_ext1_wakeup");
    const size_t terminalOutcome = deepSleepBody.find("sdLog(outcome)");
    const size_t firstInactiveCheck = deepSleepBody.find("wakeMaskIsInactive(wakeMask)");
    const size_t finalInactiveCheck = deepSleepBody.find("wakeMaskIsInactive(wakeMask)", firstInactiveCheck + 1);
    const size_t sleepStart = deepSleepBody.find("esp_deep_sleep_start()");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, wakeConfig);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, terminalOutcome);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, firstInactiveCheck);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, finalInactiveCheck);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, sleepStart);
    TEST_ASSERT_TRUE(firstInactiveCheck < wakeConfig);
    TEST_ASSERT_TRUE(wakeConfig < terminalOutcome);
    TEST_ASSERT_TRUE(terminalOutcome < finalInactiveCheck);
    TEST_ASSERT_TRUE(finalInactiveCheck < sleepStart);
    TEST_ASSERT_EQUAL_UINT32(3, countOccurrences(deepSleepBody, "releaseBacklightSleepHoldAfterAbort();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, deepSleepBody.find("ESP_EXT1_WAKEUP_ANY_LOW"));
    TEST_ASSERT_EQUAL(std::string::npos, deepSleepBody.find("ESP_EXT1_WAKEUP_ANY_HIGH"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, powerOffBody.find("planExternalPowerWake()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          powerOffBody.find("waitForPinHigh(PWR_BUTTON_GPIO, kPowerButtonReleaseWaitMs)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, powerOffBody.find("planBatteryFallbackWake(pwrPinHigh)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          powerOffBody.find("enterDeepSleep(wakeMask, sdLogEnabled, wakeMask, outcome)"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        powerOffBody.find("OUTCOME mode=deep_sleep_external_power batteryLatch=%s wake=%s trigger=active_low"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          powerOffBody.find("OUTCOME mode=deep_sleep_fallback reason=%s wake=%s trigger=active_low"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_audio_process_amp_timeout_uses_nonblocking_disable_and_only_clears_on_success);
    RUN_TEST(test_audio_tasks_abort_when_codec_init_or_amp_enable_fails);
    RUN_TEST(test_battery_critical_poweroff_checks_latch_drop_failure_with_extended_budget);
    RUN_TEST(test_battery_shutdown_readback_uses_explicit_wire_timeout_and_restores_it);
    RUN_TEST(test_deep_sleep_wake_releases_hold_with_backlight_forced_off);
    RUN_TEST(test_poweroff_outcome_is_terminal_deep_sleep_evidence);
    return UNITY_END();
}
