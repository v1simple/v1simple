/**
 * V1 Gen2 Simple Display - composition root.
 *
 * Boot selection stays here. MaintenanceRuntime and DriveRuntime own their
 * mutually exclusive lifecycle and loop behavior.
 */

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>

#include "battery_manager.h"
#include "config.h"
#include "drive_runtime.h"
#include "main_internals.h"
#include "maintenance_runtime.h"
#include "modules/event_log/product_event_log.h"
#include "modules/health/health_journal.h"
#include "runtime_coordinator.h"
#include "settings.h"
#include "storage_manager.h"
#include "v1_devices.h"
#include "v1_profiles.h"
#include "wifi_manager.h"

namespace {
constexpr uint32_t kBootSplashHoldMs = 400;
constexpr uint32_t kMinimumScanScreenDwellMs = 400;

BatteryManager batteryManager;
WiFiManager wifiManager;
ProductEventLog productEventLog;
HealthJournal healthJournal;
DriveRuntime driveRuntime(settingsManager, v1ProfileManager, v1DeviceStore, storageManager, batteryManager,
                          productEventLog, healthJournal);
MaintenanceRuntime maintenanceRuntime(
    wifiManager, settingsManager, v1ProfileManager, v1DeviceStore, storageManager, driveRuntime.display(),
    driveRuntime.preview(), driveRuntime.power(), batteryManager, driveRuntime.touch(), driveRuntime.ble(), driveRuntime.parser(),
    driveRuntime.autoPush(), driveRuntime.obd(), driveRuntime.speed(), driveRuntime.gps(), driveRuntime.quiet(),
    productEventLog, healthJournal, driveRuntime.state());

bool& loopWatchdogRegistered() {
    static bool registered = false;
    return registered;
}

void registerMainLoopTaskWatchdog() {
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
        loopWatchdogRegistered() = true;
        return;
    }
    const esp_err_t result = esp_task_wdt_add(nullptr);
    loopWatchdogRegistered() = result == ESP_OK;
    if (loopWatchdogRegistered()) {
        Serial.println("[WDT] Main loop task registered");
    } else {
        Serial.printf("[WDT] WARN: main loop task registration failed: %d\n", static_cast<int>(result));
    }
}

class MainLoopWatchdogFeedOnExit {
  public:
    ~MainLoopWatchdogFeedOnExit() {
        if (loopWatchdogRegistered()) {
            (void)esp_task_wdt_reset();
        }
    }
};

void logBootCheckpoint(const char* label, uint32_t setupStartMs) {
    Serial.printf("[BootTiming] checkpoint=%s total=%lu\n", label,
                  static_cast<unsigned long>(millis() - setupStartMs));
}

void logBootStage(const char* stage, uint32_t setupStartMs, uint32_t& stageStartedMs) {
    const uint32_t nowMs = millis();
    Serial.printf("[Boot] stage=%s delta=%lu total=%lu\n", stage,
                  static_cast<unsigned long>(nowMs - stageStartedMs),
                  static_cast<unsigned long>(nowMs - setupStartMs));
    stageStartedMs = nowMs;
}

esp_reset_reason_t initializeResetReason(uint32_t setupStartMs) {
    Serial.println("\n===================================");
    Serial.println("V1 Gen2 Simple Display");
    Serial.println("Firmware: " FIRMWARE_VERSION);
    Serial.println("[Build] core-only");
    Serial.print("Board: ");
    Serial.println(DISPLAY_NAME);

    const esp_reset_reason_t resetReason = esp_reset_reason();
    Serial.printf("Reset reason: %d ", resetReason);
    if (resetReason == ESP_RST_SW || resetReason == ESP_RST_UNKNOWN) {
        Serial.println("(SW/Upload - will clear BLE bonds for clean reconnect)");
    } else if (resetReason == ESP_RST_POWERON) {
        Serial.println("(Power-on)");
    } else if (resetReason == ESP_RST_DEEPSLEEP) {
        Serial.println("(Wake from deep sleep)");
    } else {
        Serial.printf("(Other: %d)\n", resetReason);
    }
    Serial.println("===================================\n");
    Serial.printf("[BootTiming] reset=%s (%d)\n", resetReasonToString(resetReason), static_cast<int>(resetReason));

    if (resetReason == ESP_RST_DEEPSLEEP) {
        logBootCheckpoint("wake_deepsleep", setupStartMs);
        const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        const char* causeName = "unknown";
        switch (cause) {
        case ESP_SLEEP_WAKEUP_EXT1:
            causeName = "ext1";
            break;
        case ESP_SLEEP_WAKEUP_TIMER:
            causeName = "timer";
            break;
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            causeName = "touchpad";
            break;
        case ESP_SLEEP_WAKEUP_ULP:
            causeName = "ulp";
            break;
        case ESP_SLEEP_WAKEUP_GPIO:
            causeName = "gpio";
            break;
        case ESP_SLEEP_WAKEUP_UART:
            causeName = "uart";
            break;
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            causeName = "undefined";
            break;
        default:
            break;
        }
        Serial.printf("[DeepSleep] wake_cause=%s (%d) ext1_status=0x%016llX\n", causeName,
                      static_cast<int>(cause),
                      static_cast<unsigned long long>(esp_sleep_get_ext1_wakeup_status()));
    }

    driveRuntime.state().activeScanScreenDwellMs = kMinimumScanScreenDwellMs;
    Serial.printf("[BootTiming] scan_dwell_target_ms=%lu\n",
                  static_cast<unsigned long>(driveRuntime.state().activeScanScreenDwellMs));
    driveRuntime.resetConnectionCadence();
    return resetReason;
}

void initializeSharedHardware(esp_reset_reason_t resetReason, bool maintenanceBoot,
                              uint32_t setupStartMs, uint32_t& stageStartedMs) {
    const bool psramOk = psramFound();
    Serial.printf("[Memory] PSRAM: found=%s total=%lu free=%lu largest=%lu\n", psramOk ? "yes" : "no",
                  static_cast<unsigned long>(ESP.getPsramSize()), static_cast<unsigned long>(ESP.getFreePsram()),
                  static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    logBootStage("preflight", setupStartMs, stageStartedMs);

    batteryManager.begin();
    logBootStage("battery", setupStartMs, stageStartedMs);

    driveRuntime.display().setObdRuntimeModule(&driveRuntime.obd());
    driveRuntime.display().setAlpRuntimeModule(&driveRuntime.alp());
    driveRuntime.display().setSystemStatusSources(batteryManager, wifiManager, driveRuntime.gps());
    if (!driveRuntime.display().begin()) {
        Serial.println("Display initialization failed!");
        fatalBootError(driveRuntime.display(), "Display init failed", false);
    }
    driveRuntime.state().bootReadyDeadlineMs = millis() + 5000;

    constexpr uint32_t kPostDisplaySettleMs = 10;
    delay(kPostDisplaySettleMs);
    Serial.printf("[BootTiming] post_display_settle_ms=%lu\n",
                  static_cast<unsigned long>(kPostDisplaySettleMs));
    logBootStage("display", setupStartMs, stageStartedMs);

    settingsManager.begin();
    driveRuntime.power().begin(&batteryManager, &driveRuntime.display(), &settingsManager);
    driveRuntime.power().logStartupStatus();
    logBootStage("settings", setupStartMs, stageStartedMs);

    if (maintenanceBoot) {
        logBootCheckpoint("maintenance_ui_begin", setupStartMs);
        const uint32_t startedMs = millis();
        driveRuntime.display().showMaintenanceMode();
        Serial.printf("[BootTiming] maintenance_ui_ms=%lu\n",
                      static_cast<unsigned long>(millis() - startedMs));
    } else if (resetReason == ESP_RST_POWERON) {
        logBootCheckpoint("splash_begin", setupStartMs);
        const uint32_t startedMs = millis();
        driveRuntime.display().showBootSplash();
        Serial.printf("[BootTiming] splash_call_ms=%lu\n",
                      static_cast<unsigned long>(millis() - startedMs));
        driveRuntime.state().bootSplashHoldActive = true;
        driveRuntime.state().bootSplashHoldUntilMs = millis() + kBootSplashHoldMs;
    } else {
        logBootCheckpoint("wake_ui_scan_begin", setupStartMs);
        const uint32_t startedMs = millis();
        driveRuntime.showInitialScanningScreen();
        Serial.printf("[BootTiming] wake_ui_scan_ms=%lu\n",
                      static_cast<unsigned long>(millis() - startedMs));
    }
    logBootStage("boot_ui", setupStartMs, stageStartedMs);
    driveRuntime.preview().begin(&driveRuntime.display());
}
} // namespace

void setup() {
    const uint32_t setupStartMs = millis();
    uint32_t stageStartedMs = setupStartMs;
    initializeEarlyBootDiagnostics();

    const bool maintenanceBoot = readAndClearMaintenanceBootRequest();
    driveRuntime.state().maintenanceBootActive = maintenanceBoot;
    const esp_reset_reason_t resetReason = initializeResetReason(setupStartMs);
    initializeSharedHardware(resetReason, maintenanceBoot, setupStartMs, stageStartedMs);

    MainRuntimeCoordinator::start(maintenanceBoot, setupStartMs, stageStartedMs, resetReason,
                                  driveRuntime, maintenanceRuntime);
    registerMainLoopTaskWatchdog();
}

void loop() {
    MainLoopWatchdogFeedOnExit watchdogFeed;
    MainRuntimeCoordinator::tick(driveRuntime, maintenanceRuntime, []() { return millis(); });
}
