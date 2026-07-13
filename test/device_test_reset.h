/**
 * device_test_reset.h — Auto-restart after device tests finish.
 *
 * Problem: On ESP32-S3 with USB CDC, the USB stack can degrade after tests
 * (WiFi, I2C, FreeRTOS task manipulation). If the test firmware just loops
 * after UNITY_END(), the USB CDC port disappears from the host, preventing
 * upload of the next test suite.
 *
 * Solution: After tests complete, call esp_restart() to reboot cleanly with
 * a fresh USB CDC stack. On the second boot, an RTC_NOINIT_ATTR flag tells
 * setup() to skip tests and just keep USB alive via loop().
 *
 * Additional fix: All tests wait for the PlatformIO host to open the serial
 * port before printing any output, preventing USB CDC buffer overflow when
 * tests finish before PlatformIO connects.
 *
 * Usage in each test file:
 *
 *   #include "../device_test_reset.h"
 *
 *   void setup() {
 *       if (deviceTestSetup("test_device_xxx")) return;
 *       UNITY_BEGIN();
 *       // ... RUN_TESTs ...
 *       UNITY_END();
 *       deviceTestFinish();
 *   }
 *
 *   void loop() {
 *       delay(100);
 *   }
 */
#pragma once

#include <Arduino.h>
#include <esp_system.h>

static constexpr uint32_t DEVICE_TEST_DONE_MAGIC = 0xBEEFCAFE;

// Survives software reset (esp_restart) but NOT power-cycle / flash-erase.
static RTC_NOINIT_ATTR uint32_t _deviceTestDoneFlag;
static const char* _deviceTestSuiteName = "";

static inline const char* deviceTestGitSha() {
#ifdef GIT_SHA
    return GIT_SHA;
#else
    return "";
#endif
}

static inline void deviceTestMetricU32(const char* metric,
                                       const char* sample,
                                       uint32_t value,
                                       const char* unit) {
    Serial.printf(
        "{\"schema_version\":1,\"run_id\":\"\",\"git_sha\":\"%s\",\"run_kind\":\"device_suite\","
        "\"suite_or_profile\":\"%s\",\"metric\":\"%s\",\"sample\":\"%s\",\"value\":%lu,"
        "\"unit\":\"%s\",\"tags\":{}}\n",
        deviceTestGitSha(),
        _deviceTestSuiteName,
        metric,
        sample,
        (unsigned long)value,
        unit
    );
}

static inline void deviceTestMetricI32(const char* metric,
                                       const char* sample,
                                       int32_t value,
                                       const char* unit) {
    Serial.printf(
        "{\"schema_version\":1,\"run_id\":\"\",\"git_sha\":\"%s\",\"run_kind\":\"device_suite\","
        "\"suite_or_profile\":\"%s\",\"metric\":\"%s\",\"sample\":\"%s\",\"value\":%ld,"
        "\"unit\":\"%s\",\"tags\":{}}\n",
        deviceTestGitSha(),
        _deviceTestSuiteName,
        metric,
        sample,
        (long)value,
        unit
    );
}

static inline void deviceTestMetricBool(const char* metric,
                                        const char* sample,
                                        bool value) {
    deviceTestMetricU32(metric, sample, value ? 1U : 0U, "bool");
}

/**
 * Call at the very start of setup().
 * - On a post-test reboot: initialises serial, prints keepalive msg, returns true.
 *   Caller should `return;` so loop() keeps USB alive for next upload.
 * - On a normal boot: initialises serial, waits up to 5 s for PlatformIO host
 *   to open the port, prints a diagnostic banner, returns false.
 */
static inline bool deviceTestSetup(const char* suiteName) {
    _deviceTestSuiteName = suiteName;
    // Post-test reboot path — skip tests, keep USB alive
    if (_deviceTestDoneFlag == DEVICE_TEST_DONE_MAGIC) {
        _deviceTestDoneFlag = 0;  // Clear for next firmware upload
        Serial.begin(115200);
        delay(500);
        Serial.println("[device_test] Post-test reboot — USB CDC alive for next upload.");
        return true;
    }

    // Normal boot path — init serial and WAIT for PlatformIO to connect
    Serial.begin(115200);
    const unsigned long startMs = millis();
    while (!Serial && (millis() - startMs) < 8000) {
        delay(10);
    }
    delay(500);  // Extra settle time after host connects
    Serial.printf("\n[%s] serial_ready=%d uptime=%lu ms free_heap=%lu\n",
                  suiteName,
                  Serial ? 1 : 0,
                  (unsigned long)millis(),
                  (unsigned long)ESP.getFreeHeap());
    return false;
}

/**
 * Call AFTER UNITY_END() (and after any per-suite cleanup like wdt_delete).
 * Flushes serial so PlatformIO reads all results, then reboots the device.
 */
static inline void deviceTestFinish() {
    Serial.println("[device_test] Tests done — restarting to restore USB CDC...");
    Serial.flush();
    delay(2000);  // Give PlatformIO time to read final output
    _deviceTestDoneFlag = DEVICE_TEST_DONE_MAGIC;
    esp_restart();
    // Never reaches here
}
