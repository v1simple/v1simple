/**
 * V1 Gen2 Simple Display - Main Application
 * Target: Waveshare ESP32-S3-Touch-LCD-3.49 with Valentine1 Gen2 BLE
 *
 * Features:
 * - BLE client for V1 Gen2 radar detector
 * - BLE server proxy for companion app compatibility
 * - 3.49" LCD display with touch support
 * - WiFi web interface for configuration
 * - 3-slot auto-push profile system
 * - Tap-to-mute functionality
 * - Multiple color themes
 *
 * Architecture:
 * - FreeRTOS queue for BLE data handling
 * - Non-blocking display updates
 * - Persistent settings via Preferences
 *
 * Author: Based on Valentine Research ESP protocol
 * License: MIT
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include "main_internals.h"
#include "main_runtime_state.h"
#include "main_loop_phases.h"
#include "main_loop_wiring.h"
#include "main_runtime_wiring.h"
#include "ble_client.h"
#include "packet_parser.h"
#include "display.h"
#include "display_mode.h"
#include "wifi_manager.h"
#include "settings.h"
#include "settings_runtime_sync.h"
#include "touch_handler.h"
#include "v1_profiles.h"
#include "v1_devices.h"
#include "battery_manager.h"
#include "storage_manager.h"
#include "build_metadata.h"
#include "audio_beep.h"
#include "config.h"
#include "modules/alert_persistence/alert_persistence_module.h"
#include "modules/display/display_preview_module.h"
#include "modules/auto_push/auto_push_module.h"
#include "modules/touch/touch_ui_module.h"
#include "modules/touch/tap_gesture_module.h"
#include "modules/wifi/wifi_orchestrator_module.h"
#include "modules/wifi/wifi_maintenance_recovery_module.h"
#include "modules/power/power_module.h"
#include "modules/ble/ble_queue_module.h"
#include "modules/ble/connection_state_module.h"
#include "modules/ble/connection_runtime_module.h"
#include "modules/ble/connection_state_cadence_module.h"
#include "modules/ble/connection_state_dispatch_module.h"
#include "modules/display/display_pipeline_module.h"
#include "modules/display/display_orchestration_module.h"
#include "modules/system/system_event_bus.h"
#include "modules/system/parsed_frame_event_module.h"
#include "modules/system/periodic_maintenance_module.h"
#include "modules/system/loop_tail_module.h"
#include "modules/system/loop_ingest_module.h"
#include "modules/system/loop_display_module.h"
#include "modules/system/loop_power_touch_module.h"
#include "modules/system/loop_runtime_snapshot_module.h"
#include "modules/system/loop_connection_early_module.h"
#include "modules/system/connection_cycle_coordinator_module.h"
#include "esp_heap_caps.h"
#include "modules/voice/voice_module.h"
#include "modules/volume_fade/volume_fade_module.h"
#include "modules/quiet/quiet_coordinator_module.h"
#include "modules/display/display_restore_module.h"

#include "modules/speed/speed_source_selector.h"
#include "modules/speed_mute/speed_mute_module.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/obd/obd_ble_client.h"
#include "modules/obd/obd_settings_sync_module.h"
#include "modules/alp/alp_runtime_module.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/alp/alp_event_latch.h"
#include "modules/event_log/product_event_log.h"
#include "modules/health/health_journal.h"

#include "provider_callback_bindings.h"
#include <driver/gpio.h>
#include "display_driver.h"
#include <algorithm>
#include <cstring>

// Global objects
// ObdRuntimeModule, ObdBleClient, SpeedSourceSelector are forward-declared
// in main_globals.h. Full types are visible here from the module headers above.
#include "main_globals.h"

V1BLEClient bleClient;
PacketParser parser;
V1Display display;

TouchHandler touchHandler;
SpeedSourceSelector speedSourceSelector;

// Alert persistence module
AlertPersistenceModule alertPersistenceModule;

static constexpr unsigned long BOOT_SPLASH_HOLD_MS = 400;
static constexpr unsigned long MIN_SCAN_SCREEN_DWELL_MS = 400;
static constexpr unsigned long CONNECTION_STATE_PROCESS_MAX_GAP_MS = 1000;
MainRuntimeState mainRuntimeState;

namespace {

ProductAlpState productAlpState(uint8_t heartbeatByte1) {
    switch (heartbeatByte1) {
    case 0x01:
        return ProductAlpState::TARGETED;
    case 0x03:
        return ProductAlpState::DLI;
    case 0x04:
        return ProductAlpState::LID;
    default:
        return ProductAlpState::UNKNOWN;
    }
}

void observeAlpProductState(uint32_t nowMs) {
    const AlpStatus status = alpRuntimeModule.snapshot();
    AlpProductObservation observation{};
    observation.connected = alpRuntimeModule.isEnabled() && status.uartActive && status.lastHeartbeatMs != 0 &&
                            static_cast<uint32_t>(nowMs - status.lastHeartbeatMs) <=
                                AlpRuntimeModule::HEARTBEAT_TIMEOUT_MS;
    observation.active = status.hasLaserEvent;
    observation.state = productAlpState(status.lastHbByte1);
    observation.direction = static_cast<uint8_t>(status.laserDirection);
    observation.gun = static_cast<uint8_t>(status.lastGun);
    observation.detectGeneration = status.detectGeneration;
    memcpy(observation.detectRaw, status.detectRaw, sizeof(observation.detectRaw));
    productEventLog.observeAlp(observation, nowMs);
}

bool preservedPanicEvidencePresent(esp_reset_reason_t resetReason) {
    const bool crashReset = resetReason == ESP_RST_PANIC || resetReason == ESP_RST_INT_WDT ||
                            resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT;
    fs::FS* internalFs = storageManager.getLittleFS();
    return crashReset || (internalFs && (internalFs->exists("/panic.txt") || internalFs->exists("/panic.prev.txt")));
}

} // namespace
static bool mainLoopTaskWatchdogRegistered = false;

static void registerMainLoopTaskWatchdog() {
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
        mainLoopTaskWatchdogRegistered = true;
        return;
    }

    const esp_err_t result = esp_task_wdt_add(nullptr);
    mainLoopTaskWatchdogRegistered = result == ESP_OK;
    if (mainLoopTaskWatchdogRegistered) {
        Serial.println("[WDT] Main loop task registered");
    } else {
        Serial.printf("[WDT] WARN: main loop task registration failed: %d\n", static_cast<int>(result));
    }
}

class MainLoopWatchdogFeedOnExit {
  public:
    ~MainLoopWatchdogFeedOnExit() {
        // Feed only after a loop path completes. If the loop blocks in BLE,
        // storage, display, or maintenance work, this destructor is not
        // reached and the task watchdog can recover the device.
        if (mainLoopTaskWatchdogRegistered) {
            (void)esp_task_wdt_reset();
        }
    }
};

// Display preview driver (color demos)
DisplayPreviewModule displayPreviewModule;
ConnectionStateCadenceModule connectionStateCadenceModule;

void requestColorPreviewHold(uint32_t durationMs) {
    displayPreviewModule.requestHold(durationMs);
}
bool isDisplayPreviewRunning() {
    return displayPreviewModule.isRunning();
}
void cancelDisplayPreview() {
    displayPreviewModule.cancel();
}

DisplayMode displayMode = DisplayMode::IDLE;

// Volume fade module - reduce V1 volume after X seconds of continuous alert
VoiceModule voiceModule;
VolumeFadeModule volumeFadeModule;
QuietCoordinatorModule quietCoordinatorModule;

// Auto-push profile state machine
AutoPushModule autoPushModule;
TouchUiModule touchUiModule;
TapGestureModule tapGestureModule;
PowerModule powerModule;
BleQueueModule bleQueueModule;
ConnectionStateModule connectionStateModule;
ConnectionRuntimeModule connectionRuntimeModule;
ConnectionStateDispatchModule connectionStateDispatchModule;
DisplayPipelineModule displayPipelineModule;
AlpEventLatch alpEventLatch;
DisplayOrchestrationModule displayOrchestrationModule;
DisplayRestoreModule displayRestoreModule;
SystemEventBus systemEventBus;
PeriodicMaintenanceModule periodicMaintenanceModule;
ObdSettingsSyncModule obdSettingsSyncModule;
SpeedMuteModule speedMuteModule;
LoopTailModule loopTailModule;
LoopIngestModule loopIngestModule;
LoopDisplayModule loopDisplayModule;
LoopPowerTouchModule loopPowerTouchModule;
LoopRuntimeSnapshotModule loopRuntimeSnapshotModule;
LoopConnectionEarlyModule loopConnectionEarlyModule;
ConnectionCycleCoordinatorModule connectionCycleCoordinatorModule;

// Callback for BLE data reception - just queues data, doesn't process
// This runs in BLE task context, so we avoid SPI operations here
void onV1Data(const uint8_t* data, size_t length, uint16_t charUUID, uint32_t sessionGeneration,
              uint32_t callbackMillis) {
    bleQueueModule.onNotify(data, length, charUUID, sessionGeneration, callbackMillis);
}

template <typename StageLogger>
static void finalizeBootReadyAndBleScan(const unsigned long setupStartMs, const StageLogger& logBootStage) {
    mainRuntimeState.bootReady = true;
    bleClient.setBootReady(true);
    healthJournal.ready(millis());
    Serial.printf("[Boot] Ready gate opened at %lu ms\n", millis());

    // Absorb BLE scan-stop settle cost in setup rather than first loop iteration.
    // Keep this call after bootReady/setBootReady to avoid starving BLE state
    // transitions that gate connectionStateModule.process() and scanning UI flow.
    {
        const unsigned long absorbStartMs = millis();
        bleClient.process();
        Serial.printf("[BootTiming] ble_absorb_ms=%lu\n", millis() - absorbStartMs);
    }
    Serial.println("BLE scan active from setup path");
    logBootStage("core_pipeline");

    // Normal runtime WiFi is disabled by split-boot architecture. Explicit
    // maintenance entry reboots before WiFi starts.
    const V1Settings& wifiSettings = settingsManager.get();
    if (!wifiSettings.enableWifi) {
        Serial.println("[WiFi] Master disabled in settings — startup and loop processing skipped");
    } else {
        Serial.println("Setup complete - BLE scanning, WiFi off; BOOT long-press requests maintenance reboot");
    }
    logBootStage("wifi");
    Serial.printf("[Boot] setup total: %lu ms\n", millis() - setupStartMs);
}

template <typename CheckpointLogger>
static esp_reset_reason_t initializeResetReasonAndCadenceState(const CheckpointLogger& logBootCheckpoint) {
    Serial.println("\n===================================");
    Serial.println("V1 Gen2 Simple Display");
    Serial.println("Firmware: " FIRMWARE_VERSION);
    Serial.println("[Build] core-only");
    Serial.print("Board: ");
    Serial.println(DISPLAY_NAME);

    // Check reset reason - if firmware flash, clear BLE bonds
    esp_reset_reason_t resetReason = esp_reset_reason();
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
        logBootCheckpoint("wake_deepsleep");

        // Classify which wake source fired so field diagnostics can
        // distinguish deep-sleep wake types. Only ext1 is used (button and
        // ALP both route through ext1 after 9b49a5c5); the ext1_status hex
        // below identifies which GPIO fired within that group.
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
        const uint64_t ext1Status = esp_sleep_get_ext1_wakeup_status();
        Serial.printf("[DeepSleep] wake_cause=%s (%d) ext1_status=0x%016llX\n", causeName, static_cast<int>(cause),
                         static_cast<unsigned long long>(ext1Status));
    }
    mainRuntimeState.activeScanScreenDwellMs = MIN_SCAN_SCREEN_DWELL_MS;
    Serial.printf("[BootTiming] scan_dwell_target_ms=%lu\n", mainRuntimeState.activeScanScreenDwellMs);
    connectionStateCadenceModule.reset();
    return resetReason;
}

template <typename CheckpointLogger, typename StageLogger>
static void initializeBlePreInitAndScan(const CheckpointLogger& logBootCheckpoint, const StageLogger& logBootStage) {
    // ── BLE init + scan start ────────────────────────────────────────
    // Run AFTER SD restore/validation so BLE proxy settings reflect the
    // restored configuration during the first scan/connection attempt.
    {
        const V1Settings& blePreInitSettings = settingsManager.get();
        logBootCheckpoint("ble_preinit_begin");
        const unsigned long blePreInitStartMs = millis();
        if (!bleClient.initBLE(blePreInitSettings.proxyBLE, blePreInitSettings.proxyName.c_str())) {
            Serial.println("BLE pre-initialization failed!");
            fatalBootError("BLE pre-init failed", true);
        }
        Serial.printf("[BootTiming] ble_preinit_ms=%lu\n", millis() - blePreInitStartMs);
        logBootStage("ble_preinit");

        // Scan starts in setup; connection state-machine work still waits for
        // the boot-ready gate later in setup().
        bleClient.onDataReceived(onV1Data);
        bleClient.onV1SessionOpened(onV1SessionOpened);
        bleClient.onV1SessionClosed(onV1SessionClosed);
        bleClient.onV1ConnectImmediate(onV1ConnectImmediate);
        bleClient.onV1Connected(onV1Connected);
        logBootCheckpoint("ble_callbacks_registered");
        const V1Settings& bleScanSettings = settingsManager.get();
        Serial.printf("Starting BLE scan for V1 (proxy: %s)\n", bleScanSettings.proxyBLE ? "enabled" : "disabled");
        logBootCheckpoint("ble_scan_begin");
        const unsigned long bleScanStartMs = millis();
        if (!bleClient.begin(bleScanSettings.proxyBLE, bleScanSettings.proxyName.c_str())) {
            Serial.println("BLE scan failed to start!");
            fatalBootError("BLE scan failed", true);
        }
        Serial.printf("[BootTiming] ble_scan_start_ms=%lu\n", millis() - bleScanStartMs);
    }
}

template <typename CheckpointLogger, typename StageLogger>
static void initializePreflightDisplayAndBootUi(esp_reset_reason_t resetReason, bool maintenanceBoot,
                                                const CheckpointLogger& logBootCheckpoint,
                                                const StageLogger& logBootStage) {
    // Runtime PSRAM visibility: board metadata can differ from actual hardware.
    bool psramOk = psramFound();
    uint32_t psramTotal = static_cast<uint32_t>(ESP.getPsramSize());
    uint32_t psramFree = static_cast<uint32_t>(ESP.getFreePsram());
    uint32_t psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    Serial.printf("[Memory] PSRAM: found=%s total=%lu free=%lu largest=%lu\n", psramOk ? "yes" : "no",
                     static_cast<unsigned long>(psramTotal), static_cast<unsigned long>(psramFree),
                     static_cast<unsigned long>(psramLargest));
    logBootStage("preflight");

    // Initialize battery manager EARLY - needs to latch power on if running on battery.
    // This must happen before any long-running init to prevent shutdown.
    batteryManager.begin();
    logBootStage("battery");

    // Initialize display.
    if (!display.begin()) {
        Serial.println("Display initialization failed!");
        fatalBootError("Display init failed", false);
    }
    mainRuntimeState.bootReadyDeadlineMs = millis() + 5000;
    display.setObdRuntimeModule(&obdRuntimeModule);
    display.setAlpRuntimeModule(&alpRuntimeModule);

    // Brief post-display settle before settings init.
    constexpr unsigned long postDisplaySettleMs = 10UL;
    delay(postDisplaySettleMs);
    Serial.printf("[BootTiming] post_display_settle_ms=%lu\n", postDisplaySettleMs);
    logBootStage("display");

    // Initialize settings before showing any screens.
    settingsManager.begin();
    powerModule.begin(&batteryManager, &display, &settingsManager);
    powerModule.setShutdownPreparationCallback(prepareForShutdown, nullptr);
    powerModule.setShutdownAbortCallback(resumeAfterAbortedShutdown, nullptr);
    powerModule.logStartupStatus();
    logBootStage("settings");

    // Maintenance boot is a separate mode; do not show the normal scanning UI.
    if (maintenanceBoot) {
        logBootCheckpoint("maintenance_ui_begin");
        const unsigned long maintenanceUiStartMs = millis();
        display.showMaintenanceMode();
        Serial.printf("[BootTiming] maintenance_ui_ms=%lu\n", millis() - maintenanceUiStartMs);
    } else if (resetReason == ESP_RST_POWERON) {
        // Show boot splash only on true power-on (not crash reboots or firmware uploads).
        // True cold boot: brief non-blocking splash for immediate visual confirmation.
        logBootCheckpoint("splash_begin");
        const unsigned long splashCallStartMs = millis();
        display.showBootSplash();
        Serial.printf("[BootTiming] splash_call_ms=%lu\n", millis() - splashCallStartMs);
        mainRuntimeState.bootSplashHoldActive = true;
        mainRuntimeState.bootSplashHoldUntilMs = millis() + BOOT_SPLASH_HOLD_MS;
    } else {
        logBootCheckpoint("wake_ui_scan_begin");
        const unsigned long wakeUiStartMs = millis();
        showInitialScanningScreen();
        Serial.printf("[BootTiming] wake_ui_scan_ms=%lu\n", millis() - wakeUiStartMs);
    }
    logBootStage("boot_ui");

    // Initialize display preview driver.
    displayPreviewModule.begin(&display);
}

static constexpr unsigned long MAINTENANCE_EXIT_LONG_PRESS_MS = 4000UL;

static void servicePowerDisplayOwnership(unsigned long nowMs) {
    if (powerModule.ownsDisplayPresentation()) {
        // A preview is subordinate to the safety warning. Cancel it rather
        // than letting its timer complete unseen and then restore stale preview
        // state over the warning or its eventual authoritative replacement.
        if (displayPreviewModule.isRunning()) {
            displayPreviewModule.cancel();
        }
        (void)displayPreviewModule.consumeEnded();
        touchUiModule.suspendForPresentationOwner();
        tapGestureModule.suspendForPresentationOwner();
        return;
    }

    if (!powerModule.consumeDisplayRestoreRequest()) {
        return;
    }
    const bool restoreBrightness = powerModule.consumeDisplayBrightnessRestoreRequest();
    const auto finishRestore = [&]() {
        if (restoreBrightness) {
            const uint8_t savedBrightness = settingsManager.get().brightness;
            display.setBrightness(savedBrightness);
            Serial.printf("[Power] Restored display brightness=%u after shutdown abort\n",
                             static_cast<unsigned>(savedBrightness));
        }
    };

    if (displayPreviewModule.isRunning()) {
        displayPreviewModule.cancel();
    }
    (void)displayPreviewModule.consumeEnded();
    display.forceNextRedraw();

    if (mainRuntimeState.maintenanceBootActive) {
        const bool stationMode = wifiManager.isConnected();
        const String ip = stationMode ? wifiManager.getIPAddress() : wifiManager.getAPIPAddress();
        display.showMaintenanceMode(ip.c_str(), stationMode);
        finishRestore();
        return;
    }

    if (touchUiModule.restorePresentationIfOwned()) {
        finishRestore();
        return;
    }

    if (!displayPipelineModule.restoreCurrentOwner(static_cast<uint32_t>(nowMs))) {
        // Defensive setup/partial-wiring fallback. Production normal runtime
        // has a fully initialized display pipeline by the time loop() runs.
        display.showScanning();
        display.drawProfileIndicator(settingsManager.get().activeSlot);
    }
    finishRestore();
}

static void logMaintenanceHeapSnapshot(const char* stage) {
    const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t freeDma = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    const uint32_t largestDma =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    Serial.printf("[MaintBoot] heap stage=%s freeInternal=%lu largestInternal=%lu freeDma=%lu largestDma=%lu\n",
                     stage, static_cast<unsigned long>(freeInternal), static_cast<unsigned long>(largestInternal),
                     static_cast<unsigned long>(freeDma), static_cast<unsigned long>(largestDma));
}

template <typename StageLogger>
static void initializeMaintenanceBootFlow(const unsigned long setupStartMs, const uint32_t bootId,
                                          const esp_reset_reason_t resetReason, const StageLogger& logBootStage) {
    Serial.printf("[MaintBoot] active bootId=%lu reset=%s timeoutMs=%lu maxSessionMs=%lu\n",
                     static_cast<unsigned long>(bootId), resetReasonToString(resetReason),
                     static_cast<unsigned long>(MainRuntimePolicy::MaintenanceBootTimeoutMs),
                     static_cast<unsigned long>(MainRuntimePolicy::MaintenanceBootMaxSessionMs));

    logMaintenanceHeapSnapshot("pre_wifi");
    wifiManager.setMaintenanceBootMode(true);
    configureMaintenanceWifiServices();

    initializeTouchAndDisplayControls();
    logBootStage("maintenance_touch");

    const unsigned long wifiStartMs = millis();
    const bool wifiStarted = wifiManager.startSetupMode(false);
    Serial.printf("[MaintBoot] wifi_start ok=%s elapsedMs=%lu\n", wifiStarted ? "true" : "false",
                     static_cast<unsigned long>(millis() - wifiStartMs));
    logMaintenanceHeapSnapshot(wifiStarted ? "post_wifi" : "wifi_start_failed");
    logBootStage("maintenance_wifi");

    mainRuntimeState.bootReady = true;
    healthJournal.ready(millis());
    // Session start is immutable (it anchors the absolute cap); the deadline
    // anchor starts equal to it and is republished every loop by the policy.
    const unsigned long maintenanceSessionStartMs = millis();
    mainRuntimeState.maintenanceBootSessionStartedMs =
        (maintenanceSessionStartMs == 0) ? 1UL : maintenanceSessionStartMs;
    mainRuntimeState.maintenanceLastUiActivityMs = 0;
    mainRuntimeState.maintenanceBootStartedMs = mainRuntimeState.maintenanceBootSessionStartedMs;
    Serial.printf("[MaintBoot] setup total: %lu ms\n", millis() - setupStartMs);
}

template <typename CheckpointLogger, typename StageLogger>
static void initializeStorageToReadyFlow(esp_reset_reason_t resetReason, bool maintenanceBoot,
                                         const unsigned long setupStartMs, const CheckpointLogger& logBootCheckpoint,
                                         const StageLogger& logBootStage) {
    // ── Storage / SD mount ────────────────────────────────────────────
    initializeStorageAndProfiles();

    // Clean-shutdown detection: read + reset the v1boot marker. A false
    // result means the previous run died without calling prepareForShutdown()
    // (brownout, car-power cut, cable yank, panic).  On the first boot of a
    // brand-new device the marker is also false — that's expected and
    // indistinguishable here without further signal, so we record it as
    // "unclean" once and move on.
    const bool prevShutdownClean = readAndResetCleanShutdownMarker();
    if (!prevShutdownClean) {
        Serial.println("[Boot] Previous shutdown was UNCLEAN (no clean-shutdown marker)");
    } else {
        Serial.println("[Boot] Previous shutdown was clean");
    }

    const uint32_t bootId = nextBootId();

    HealthCounters::reset();
    (void)healthJournal.begin(storageManager, bootId, getRuntimeImageId(), resetReasonToString(resetReason),
                              prevShutdownClean, preservedPanicEvidencePresent(resetReason));
    if (!maintenanceBoot) {
        (void)productEventLog.begin(bootId, storageManager);
    }

    logBootStage("storage");

    if (maintenanceBoot) {
        // Bind the consumed one-shot request to this boot's reset identity
        // before any maintenance-mode service emits runtime diagnostics.
        logBootIdentity(bootId, resetReason);
        Serial.println("[MaintBoot] request consumed; entering maintenance boot");
        initializeMaintenanceBootFlow(setupStartMs, bootId, resetReason, logBootStage);
        return;
    }

    initializeBlePreInitAndScan(logBootCheckpoint, logBootStage);

    configureUiInteractionModules(quietCoordinatorModule);
    logBootStage("ui_modules");

    logBootSummaryAndWifiStartup(bootId, resetReason);

    initializeTouchAndDisplayControls();
    logBootStage("touch");

    configureAlertDisplayPipeline();
    configureSystemLoopModules();
    configureRuntimeModules();
    finalizeBootReadyAndBleScan(setupStartMs, logBootStage);
}

void setup() {
    const unsigned long setupStartMs = millis();
    unsigned long setupStageStartMs = setupStartMs;

    initializeEarlyBootDiagnostics();

    auto logBootStage = [&](const char* stageName) {
        const unsigned long now = millis();
        Serial.printf("[Boot] stage=%s delta=%lu total=%lu\n", stageName, now - setupStageStartMs,
                         now - setupStartMs);
        setupStageStartMs = now;
    };
    auto logBootCheckpoint = [&](const char* label) {
        const unsigned long now = millis();
        Serial.printf("[BootTiming] checkpoint=%s total=%lu\n", label, now - setupStartMs);
    };

    const bool maintenanceBoot = readAndClearMaintenanceBootRequest();
    mainRuntimeState.maintenanceBootActive = maintenanceBoot;

    esp_reset_reason_t resetReason = initializeResetReasonAndCadenceState(logBootCheckpoint);

    initializePreflightDisplayAndBootUi(resetReason, maintenanceBoot, logBootCheckpoint, logBootStage);

    initializeStorageToReadyFlow(resetReason, maintenanceBoot, setupStartMs, logBootCheckpoint, logBootStage);
    registerMainLoopTaskWatchdog();
}

void loop() {
    MainLoopWatchdogFeedOnExit watchdogFeed;
    if (mainRuntimeState.maintenanceBootActive) {
        audio_process_amp_timeout();
        const unsigned long now = millis();

        powerModule.process(now);
        servicePowerDisplayOwnership(now);
        const bool powerPresentationOwned = powerModule.ownsDisplayPresentation();
        if (!powerPresentationOwned) {
            wifiManager.process();
        }

        // The maintenance session exists to serve the web UI, so it must not
        // sit WiFi-dead for its bounded lifetime: the AP can fail to start at
        // maintenance entry, and the emergency low-SRAM stop can take the
        // service down mid-session. Neither has any other recovery until the
        // maintenance timeout reboots. Ask the recovery policy whether a
        // restart attempt is due and log every outcome so sessions are
        // diagnosable from the serial log.
        static WifiMaintenanceRecoveryModule wifiMaintenanceRecoveryModule;
        if (!powerPresentationOwned) {
            WifiMaintenanceRecoveryInput wifiRecoveryInput;
            wifiRecoveryInput.maintenanceBootActive = true;
            wifiRecoveryInput.wifiServiceReachable = wifiManager.isWifiServiceReachable();
            wifiRecoveryInput.nowMs = now;
            const WifiMaintenanceRecoveryResult wifiRecovery =
                wifiMaintenanceRecoveryModule.evaluate(wifiRecoveryInput);
            if (wifiRecovery.attemptRestart) {
                Serial.printf("[MaintBoot] wifi service down - restart attempt %lu\n",
                                 static_cast<unsigned long>(wifiRecovery.attemptNumber));
                const bool wifiRestarted = wifiManager.startSetupMode(false);
                Serial.printf("[MaintBoot] wifi_restart ok=%s\n", wifiRestarted ? "true" : "false");
            }
        }

        settingsManager.serviceDeferredPersist(static_cast<uint32_t>(now));
        settingsManager.serviceDeferredBackup(static_cast<uint32_t>(now));

        // Keep the maintenance screen's shown address current: the STA/DHCP IP
        // once we've joined a configured network, otherwise the setup AP's
        // default IP. Redraw only when it actually changes (a handful of times
        // per session) so we don't reflush the panel every loop.
        static String maintenanceShownIp;
        static bool maintenanceShownStation = false;
        const bool maintenanceStaConnected = wifiManager.isConnected();
        String maintenanceIp = maintenanceStaConnected ? wifiManager.getIPAddress() : wifiManager.getAPIPAddress();
        const bool maintenancePreviewRunning = displayPreviewModule.isRunning();
        if (!powerPresentationOwned && !maintenancePreviewRunning &&
            (maintenanceIp != maintenanceShownIp || maintenanceStaConnected != maintenanceShownStation)) {
            display.showMaintenanceMode(maintenanceIp.c_str(), maintenanceStaConnected);
        }
        maintenanceShownIp = maintenanceIp;
        maintenanceShownStation = maintenanceStaConnected;

        // Normal runtime advances previews through DisplayOrchestrationModule,
        // which is intentionally not initialized in maintenance boot. Service
        // the shared preview module here so Colors/visual API previews actually
        // render, then return ownership to the maintenance screen on expiry or
        // cancellation.
        if (!powerPresentationOwned && maintenancePreviewRunning) {
            displayPreviewModule.update();
        }
        if (!powerPresentationOwned && displayPreviewModule.consumeEnded()) {
            display.showMaintenanceMode(maintenanceIp.c_str(), maintenanceStaConnected);
        }

        static unsigned long bootButtonPressStartMs = 0;
        static bool exitRequestFired = false;
        static bool idleHeapLogged = false;
        // Anchored on the immutable session start, not the deadline anchor:
        // the deadline anchor slides with UI activity, so it would delay or
        // suppress this one-shot diagnostic on a session that is being used.
        if (!idleHeapLogged && mainRuntimeState.maintenanceBootSessionStartedMs != 0 &&
            static_cast<uint32_t>(now - mainRuntimeState.maintenanceBootSessionStartedMs) >= 5000UL) {
            idleHeapLogged = true;
            logMaintenanceHeapSnapshot("idle_5s");
        }

        const bool bootPressed = digitalRead(BOOT_BUTTON_GPIO) == LOW;
        if (powerPresentationOwned) {
            bootButtonPressStartMs = 0;
            exitRequestFired = false;
        } else if (bootPressed && bootButtonPressStartMs == 0) {
            bootButtonPressStartMs = now == 0 ? 1 : now;
            exitRequestFired = false;
        } else if (!bootPressed) {
            bootButtonPressStartMs = 0;
            exitRequestFired = false;
        } else if (!exitRequestFired && bootButtonPressStartMs != 0 &&
                   (now - bootButtonPressStartMs) >= MAINTENANCE_EXIT_LONG_PRESS_MS) {
            exitRequestFired = true;
            Serial.println("[MaintBoot] BOOT long-press exit -> rebooting normal runtime");
            const bool persistenceSafe = completeLoggingForControlledRestart();
            if (persistenceSafe) {
                settingsManager.save();
                markCleanShutdown();
            } else {
                Serial.println("[MaintBoot] WARN: exit continuing without final persistence writes");
            }
            ESP.restart();
        }

        // Delegate maintenance-session deadlines to the host-tested policy.
        // WiFiManager publishes recent activity as a predicate, so the loop
        // records the observation time before evaluating the policy.
        if (wifiManager.isUiActive(MainRuntimePolicy::MaintenanceUiActivityProbeMs)) {
            mainRuntimeState.maintenanceLastUiActivityMs = (now == 0) ? 1UL : now;
        }

        MainRuntimePolicy::MaintenanceSessionInput maintenanceSessionInput;
        maintenanceSessionInput.nowMs = static_cast<uint32_t>(now);
        maintenanceSessionInput.sessionStartedMs =
            static_cast<uint32_t>(mainRuntimeState.maintenanceBootSessionStartedMs);
        maintenanceSessionInput.lastUiActivityMs = static_cast<uint32_t>(mainRuntimeState.maintenanceLastUiActivityMs);
        maintenanceSessionInput.sessionActive = mainRuntimeState.maintenanceBootSessionStartedMs != 0;
        const MainRuntimePolicy::MaintenanceSessionDecision maintenanceSession =
            MainRuntimePolicy::evaluateMaintenanceSession(maintenanceSessionInput);

        // Republish the deadline anchor so /api/status keeps reporting a
        // countdown that agrees with the device. See the contract note on
        // MaintenanceSessionDecision::deadlineAnchorMs.
        mainRuntimeState.maintenanceBootStartedMs = maintenanceSession.deadlineAnchorMs;

        if (maintenanceSession.shouldReboot) {
            Serial.printf("[MaintBoot] timeout reason=%s sessionMs=%lu idleMs=%lu -> rebooting normal runtime\n",
                             maintenanceSession.maxSessionReached ? "max_session" : "idle",
                             static_cast<unsigned long>(maintenanceSession.elapsedSinceStartMs),
                             static_cast<unsigned long>(maintenanceSession.elapsedSinceActivityMs));
            const bool persistenceSafe = completeLoggingForControlledRestart();
            if (persistenceSafe) {
                settingsManager.save();
                markCleanShutdown();
            } else {
                Serial.println("[MaintBoot] WARN: timeout restart continuing without final persistence writes");
            }
            ESP.restart();
        }

        // Keep lower-priority FreeRTOS work moving
        // while maintenance Wi-Fi serving owns the short-circuit loop path.
        vTaskDelay(pdMS_TO_TICKS(1));
        return;
    }

    unsigned long loopStartUs = micros();
    audio_process_amp_timeout();
    unsigned long now = millis();
    LoopConnectionEarlyContext loopConnectionEarlyContext;
    loopConnectionEarlyContext.nowMs = now;
    loopConnectionEarlyContext.nowUs = micros();
    loopConnectionEarlyContext.lastLoopUs = mainRuntimeState.lastLoopUs;
    loopConnectionEarlyContext.bootSplashHoldActive = mainRuntimeState.bootSplashHoldActive;
    loopConnectionEarlyContext.bootSplashHoldUntilMs = mainRuntimeState.bootSplashHoldUntilMs;
    loopConnectionEarlyContext.initialScanningScreenShown = mainRuntimeState.initialScanningScreenShown;
    loopConnectionEarlyContext.presentationSuppressed = powerModule.ownsDisplayPresentation();
    const LoopConnectionEarlyResult loopConnectionEarlyValues =
        loopConnectionEarlyModule.process(loopConnectionEarlyContext);

    mainRuntimeState.bootSplashHoldActive = loopConnectionEarlyValues.bootSplashHoldActive;
    mainRuntimeState.initialScanningScreenShown = loopConnectionEarlyValues.initialScanningScreenShown;

    bool bleConnectedNow = loopConnectionEarlyValues.bleConnectedNow;
    bool bleBackpressure = loopConnectionEarlyValues.bleBackpressure;
    bool skipNonCoreThisLoop = loopConnectionEarlyValues.skipNonCoreThisLoop;
    bool overloadThisLoop = loopConnectionEarlyValues.overloadThisLoop;

    // Process battery/power and touch UI.
    const bool settingsEarlyReturn = shouldReturnEarlyFromLoopPowerTouchPhase(now);
    servicePowerDisplayOwnership(now);
    const bool powerPresentationOwned = powerModule.ownsDisplayPresentation();
    bool alpProcessedThisLoop = false;
    if (settingsEarlyReturn) {
        // Valentine's Law (docs/VALENTINE_PHILOSOPHY.md, screen/speaker
        // contract): an alert owns the screen; the settings sliders may not
        // hold it against a live alert. V1 alert rows stay fresh through the
        // forced tail BLE drain on the prior loop, and ALP detection is
        // serviced here so a laser hit is still seen while the sliders own the
        // screen. Warm-up sessions never preempt because the named deviation
        // withholds them from currentEvent(). When a live alert is present, the
        // menu closes, retaining the in-progress adjustments, and this loop
        // falls through so the alert renders immediately; the menu is not
        // restored when the alert clears.
        //
        // process() is once-per-tick (same-tick heartbeat-resume suppression),
        // so the fall-through path must not run it a second time below.
        alpRuntimeModule.process(now);
        alpProcessedThisLoop = true;
        AlertData v1Priority;
        const bool v1LiveAlert = parser.hasAlerts() && parser.getRenderablePriorityAlert(v1Priority);
        const bool alpLiveAlert = alpRuntimeModule.ownsLaserDisplay() && alpRuntimeModule.currentEvent().active;
        const bool preempted = (v1LiveAlert || alpLiveAlert) && touchUiModule.preemptForLiveAlert();
        if (!preempted) {
            observeAlpProductState(static_cast<uint32_t>(now));
            mainRuntimeState.lastLoopUs = processLoopSettingsEarlyReturnPhase(now, loopStartUs, bleConnectedNow);
            return; // Skip normal loop processing while in settings mode.
        }
    }

    const LoopIngestPhaseValues loopIngestValues =
        processLoopIngestPhase(now, mainRuntimeState.bootReady, mainRuntimeState.bootReadyDeadlineMs,
                               skipNonCoreThisLoop, overloadThisLoop, powerPresentationOwned);
    mainRuntimeState.bootReady = loopIngestValues.bootReady;
    bleBackpressure = loopIngestValues.bleBackpressure;
    const bool skipLateNonCoreThisLoop = loopIngestValues.skipLateNonCoreThisLoop;
    const bool overloadLateThisLoop = loopIngestValues.overloadLateThisLoop;

    // Coordinator-owned OBD admission/arbitration is live. Proxy/WiFi teardown and
    // ownership transfer remain phased in separately.
    {
        const ObdRuntimeStatus obdStatus = obdRuntimeModule.snapshot(now);
        const V1Settings& currentSettings = settingsManager.get();
        bool v1VerifyPushMatchEdge = bleClient.consumeVerifyPushMatchEdge();
        const CycleContext cycleContext{
            now,
            mainRuntimeState.bootReady,
            bleConnectedNow,
            currentSettings.autoPushEnabled,
            v1VerifyPushMatchEdge,
            bleClient.lastV1ConnectionEventMs(),
            obdStatus.enabled,
            obdStatus.savedAddressValid,
            obdStatus.connected,
            obdStatus.state,
            obdStatus.speedValid,
            currentSettings.proxyBLE && bleClient.isProxyEnabled(),
            bleClient.isProxyAdvertising(),
            bleClient.isProxyClientConnected(),
            bleClient.hasProxyClientConnectedThisBoot(),
            currentSettings.obdScanWindowMs,
            currentSettings.obdRetryIntervalMs,
            currentSettings.proxyOpenWindowMs,
            currentSettings.v1SettleQuietMs,
            currentSettings.v1SettleFallbackMs,
            currentSettings.cycleTeardownAckTimeoutMs,
        };
        connectionCycleCoordinatorModule.update(cycleContext);
        ObdBleArbitrationRequest bleArbitrationRequest = connectionCycleCoordinatorModule.arbitrationRequest();
        if (obdStatus.manualScanPending) {
            bleArbitrationRequest = ObdBleArbitrationRequest::PREEMPT_PROXY_FOR_MANUAL_SCAN;
        }
        const bool proxyModeEnabled = currentSettings.proxyBLE && bleClient.isProxyEnabled();
        bleClient.setConnectionCycleProxyPolicy(
            proxyModeEnabled && connectionCycleCoordinatorModule.proxyAdvertisingAllowed(),
            proxyModeEnabled && connectionCycleCoordinatorModule.proxyKeepConnectionAllowed());
        bleClient.setObdBleArbitrationRequest(bleArbitrationRequest);
    }

    // Refresh speed inputs before display so the current loop sees the latest OBD state.
    {
        if (bleClient.isProxyClientConnected()) {
            obdRuntimeModule.stopActiveScan();
            obdRuntimeModule.cancelPendingConnect();
        }
        const ObdConnectionState obdStateBeforeUpdate = obdRuntimeModule.getState();
        const bool obdRetryAllowedNow = connectionCycleCoordinatorModule.obdRetryAllowed(now);
        const ObdBleContext obdBleContext{
            mainRuntimeState.bootReady,
            bleConnectedNow,
            !bleClient.isScanning(),
            bleClient.isConnectBurstSettling(),
            bleClient.isProxyAdvertising(),
            bleClient.isProxyClientConnected(),
            bleClient.isConnectInProgress(),
            connectionCycleCoordinatorModule.obdScanAllowed(),
            connectionCycleCoordinatorModule.obdConnectAllowed(),
            obdRetryAllowedNow,
        };
        obdRuntimeModule.update(now, obdBleContext);
        const ObdConnectionState obdStateAfterUpdate = obdRuntimeModule.getState();
        if (obdRetryAllowedNow &&
            (obdStateBeforeUpdate == ObdConnectionState::DISCONNECTED ||
             obdStateBeforeUpdate == ObdConnectionState::ECU_IDLE) &&
            obdStateAfterUpdate == ObdConnectionState::CONNECTING) {
            connectionCycleCoordinatorModule.recordObdRetryAttempt(now);
        }
    }

    // ALP UART listener — drain and parse ALP serial data.
    // Runs every loop; process() is a no-op when disabled. Skipped only when
    // the settings-preemption path above already processed this tick.
    if (!alpProcessedThisLoop) {
        alpRuntimeModule.process(now);
    }
    observeAlpProductState(static_cast<uint32_t>(now));
    const bool alpLiveAlert = alpRuntimeModule.ownsLaserDisplay() && alpRuntimeModule.currentEvent().active;
    if (alpLiveAlert && displayPreviewModule.isRunning()) {
        // V1 alerts cancel previews in BleQueueModule. ALP arrives on its own
        // UART path, so mirror that ownership handoff here before the parsed
        // ALP event reaches the display pipeline later in this loop.
        displayPreviewModule.cancel();
    }
    {
        const AlpStatus alpStatus = alpRuntimeModule.snapshot();
        const bool alpSignalActiveNow =
            alpRuntimeModule.isEnabled() && alpStatus.uartActive && alpStatus.lastHeartbeatMs != 0 &&
            (static_cast<uint32_t>(now - alpStatus.lastHeartbeatMs) <= AlpRuntimeModule::HEARTBEAT_TIMEOUT_MS);
        if (alpSignalActiveNow != mainRuntimeState.alpSignalActive) {
            powerModule.onAlpSignalChange(alpSignalActiveNow);
            mainRuntimeState.alpSignalActive = alpSignalActiveNow;
        }
    }

    gpsRuntimeModule.update(now);
    speedSourceSelector.update(now);
    {
        const V1Settings& s = settingsManager.get();
        speedMuteModule.syncSettings(s.speedMuteEnabled, s.speedMuteThresholdMph, s.speedMuteHysteresisMph,
                                     s.speedMuteVolume, s.speedMuteVoice);
        const SpeedSelection speed = speedSourceSelector.selectedSpeed();
        const bool speedValid = speed.valid;
        speedMuteModule.update(speed.speedMph, speedValid, now);
    }

    // No overload guard: per-element render caches make unchanged-frame draws cheap;
    // fade/debounce/gap-recovery remain microsecond-cheap and must run every frame.
    processLoopDisplayPhase(now, mainRuntimeState.bootSplashHoldActive, overloadLateThisLoop, powerPresentationOwned);

    const LoopRuntimeSnapshotValues loopRuntimeSnapshotValues =
        loopRuntimeSnapshotModule.process(LoopRuntimeSnapshotContext{});

    const LoopFinalizePhaseValues loopFinalizeValues =
        processLoopFinalizePhase(mainRuntimeState.bootSplashHoldActive,
                                 loopRuntimeSnapshotValues.displayPreviewRunning || powerPresentationOwned,
                                 bleBackpressure, overloadLateThisLoop, mainRuntimeState.activeScanScreenDwellMs,
                                 CONNECTION_STATE_PROCESS_MAX_GAP_MS, loopStartUs);
    now = loopFinalizeValues.dispatchNowMs;
    bleConnectedNow = loopFinalizeValues.bleConnectedNow;
    mainRuntimeState.lastLoopUs = loopFinalizeValues.lastLoopUs;

}
