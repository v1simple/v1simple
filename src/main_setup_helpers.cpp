/**
 * main_setup_helpers.cpp — Setup-time helper functions extracted from main.cpp.
 *
 * Keeps main.cpp focused on setup()/loop() orchestration while preserving
 * existing behavior and call ordering.
 */

#include "main_internals.h"
#include "main_globals.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <algorithm>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "audio_beep.h"
#include "battery_manager.h"
#include "ble_bond_backup_writer.h"
#include "ble_client.h"
#include "display.h"
#include "display_mode.h"
#include "packet_parser.h"
#include "perf_sd_logger.h"
#include "modules/alp/alp_sd_logger.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/gps/gps_publishers.h"
#include "settings.h"
#include "settings_internals.h"
#include "settings_runtime_sync.h"
#include "storage_manager.h"
#include "touch_handler.h"
#include "v1_devices.h"
#include "v1_profiles.h"
#include "wifi_manager.h"
#include "modules/auto_push/auto_push_module.h"
#include "modules/alert_persistence/alert_persistence_module.h"
#include "modules/obd/obd_ble_client.h"
#include "modules/perf/debug_macros.h"
#include "modules/touch/tap_gesture_module.h"
#include <driver/gpio.h>

namespace {

void feedLoopTaskWatchdogDuringShutdown() {
    // Graceful shutdown is an intentional sequence of individually bounded
    // drains. Feed between those steps so their cumulative budget cannot trip
    // the 5-second loop-task watchdog; a single stuck step remains detectable.
    (void)esp_task_wdt_reset();
}

struct V1ConnectedAutoPushSelection {
    int activeSlotIndex = 0;
    String connectedAddress;
    uint8_t deviceDefaultProfile = 0;
    int selectedSlotIndex = 0;
};

V1ConnectedAutoPushSelection resolveV1ConnectedAutoPushSelection(const V1Settings& settings) {
    V1ConnectedAutoPushSelection selection;
    selection.activeSlotIndex = std::max(0, std::min(2, settings.activeSlot));
    selection.selectedSlotIndex = selection.activeSlotIndex;

    NimBLEAddress connected = bleClient.getConnectedAddress();
    if (!connected.isNull()) {
        selection.connectedAddress = normalizeV1DeviceAddress(String(connected.toString().c_str()));
    }
    if (selection.connectedAddress.length() == 0) {
        selection.connectedAddress = normalizeV1DeviceAddress(settings.lastV1Address);
    }

    if (selection.connectedAddress.length() > 0 && v1DeviceStore.isReady()) {
        v1DeviceStore.touchDeviceInMemory(selection.connectedAddress);
        selection.deviceDefaultProfile = v1DeviceStore.getDeviceDefaultProfile(selection.connectedAddress);
        if (selection.deviceDefaultProfile >= 1 && selection.deviceDefaultProfile <= 3) {
            selection.selectedSlotIndex = static_cast<int>(selection.deviceDefaultProfile) - 1;
        }
    }

    return selection;
}

}  // namespace

void prepareForShutdown(void* /*context*/) {
    feedLoopTaskWatchdogDuringShutdown();

    if (wifiManager.isWifiServiceActive()) {
        Serial.println("[Battery] Stopping WiFi before shutdown flush...");
        wifiManager.stopSetupMode(true, "poweroff");
        delay(100);
        feedLoopTaskWatchdogDuringShutdown();
    }

    // ── BLE teardown ─────────────────────────────────────────────────
    // Disconnect all BLE peripherals BEFORE power-off so remote devices
    // (V1, OBDLink, ALP) see a proper GAP disconnect and release their
    // connection slots.  Without this, the next boot starts a fresh
    // NimBLE stack while the remote side still holds a stale link,
    // blocking reconnection until the remote's supervision timeout fires
    // (which may never happen on some devices).
    //
    // NimBLEDevice::deinit() is banned (see ble_internals.h) — disconnect
    // only; the stack is destroyed moments later by power-off anyway.
    Serial.println("[Battery] Disconnecting BLE peripherals before shutdown...");
    bleClient.disconnect();
    obdBleClient.disconnect();
    // Stop any active scan so the controller isn't mid-operation at power-off.
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        pScan->stop();
    }
    delay(50);  // Brief settle for disconnect packets to transmit
    feedLoopTaskWatchdogDuringShutdown();

    // Drain SD loggers (car power mode shutdown)
    Serial.println("[Battery] Draining SD loggers before shutdown...");
    perfSdLogger.drainAndClose(500);
    feedLoopTaskWatchdogDuringShutdown();
    alpSdLogger.drainAndClose(500);
    feedLoopTaskWatchdogDuringShutdown();

    // Drain the best-effort bond snapshot before the final synchronous
    // settings write takes exclusive ownership of SD.
    shutdownBleBondBackupWriter(1500);
    feedLoopTaskWatchdogDuringShutdown();

    Serial.println("[Battery] Saving settings...");
    settingsManager.save();
    feedLoopTaskWatchdogDuringShutdown();

    Serial.println("[Battery] Forcing final SD settings backup...");
    settingsManager.backupToSD();
    feedLoopTaskWatchdogDuringShutdown();

    // Stop the deferred-backup writer task now that the final synchronous
    // backup has landed. Without this signal, the writer keeps polling and
    // would emit failed-write logs once the SD is torn down.
    shutdownDeferredSettingsBackupWriter(1500);
    feedLoopTaskWatchdogDuringShutdown();

    // Mark this shutdown as clean LAST.  If anything below fails or the
    // user yanks power mid-teardown we want the next boot to still see
    // "unclean" and record the integrity event.
    Serial.println("[Battery] Writing clean-shutdown marker...");
    markCleanShutdown();
}

void onV1ConnectImmediate() {
    mainRuntimeState.v1ConnectedAtMs = millis();

    // Start a new perf CSV session so scoring tools can isolate
    // V1-connected data from idle boot noise.
    perfSdLogger.startNewSession();
}

void onV1Connected() {
    const V1Settings& s = settingsManager.get();
    const V1ConnectedAutoPushSelection selection = resolveV1ConnectedAutoPushSelection(s);

    const AutoPushSlot& slot = settingsManager.getSlot(selection.selectedSlotIndex);
    SerialLog.printf("[AutoPush] onV1Connected autoPush=%s activeSlot=%d selectedSlot=%d defaultProfile=%u addr='%s' profile='%s' mode=%d\n",
                     s.autoPushEnabled ? "on" : "off",
                     selection.activeSlotIndex,
                     selection.selectedSlotIndex,
                     static_cast<unsigned>(selection.deviceDefaultProfile),
                     selection.connectedAddress.c_str(),
                     slot.profileName.c_str(),
                     static_cast<int>(slot.mode));
    if (!s.autoPushEnabled) {
        return;
    }

    display.setProfileIndicatorSlot(selection.selectedSlotIndex);
    const auto queueResult = autoPushModule.queueSlotPush(selection.selectedSlotIndex,
                                                          false,
                                                          false);
    (void)queueResult;
}

void initializeStorageAndProfiles() {
    // Mount storage (SD if available, else LittleFS) for profiles and settings.
    SerialLog.println("[Setup] Mounting storage...");
    if (storageManager.begin()) {
        SerialLog.printf("[Setup] Storage ready: %s\n", storageManager.statusText().c_str());
        v1ProfileManager.begin(storageManager.getFilesystem(), storageManager.getLittleFS());
        v1DeviceStore.begin(storageManager.getFilesystem(), storageManager.getLittleFS());
        if (!mainRuntimeState.maintenanceBootActive) {
            audio_init_buffers();
            audio_init_sd();
        } else {
            SerialLog.println("[Setup] Maintenance boot: skipping audio buffer/voice init");
        }

        // Retry settings restore now that SD is mounted
        // (settings.begin() runs before storage, so restore may have failed)
        if (settingsManager.checkAndRestoreFromSD()) {
            // Settings were restored from SD - update display with restored brightness.
            display.setBrightness(settingsManager.get().brightness);
        }

        const String restoredLastKnownV1 = normalizeV1DeviceAddress(settingsManager.get().lastV1Address);
        if (restoredLastKnownV1.length() > 0 && v1DeviceStore.isReady()) {
            v1DeviceStore.upsertDevice(restoredLastKnownV1);
        }

        // Validate profile references in auto-push slots.
        // Clear references to profiles that don't exist.
        settingsManager.validateProfileReferences(v1ProfileManager);
    } else {
        SerialLog.println("[Setup] Storage unavailable - profiles will be disabled");
    }
}

uint32_t initializeBootPerformanceLoggers() {
    const uint32_t bootId = nextBootId();
    const uint32_t bootToken = static_cast<uint32_t>(esp_random());
    perfSdLogger.setBootId(bootId, bootToken);

    // Standalone perf CSV loggers (SD only).
    const bool sdEnabled = storageManager.isReady() && storageManager.isSDCard();
    perfSdLogger.begin(sdEnabled);
    if (perfSdLogger.isEnabled()) {
        SerialLog.printf("[PERF] SD logger enabled (%s)\n", perfSdLogger.csvPath());
    } else {
        SerialLog.println("[PERF] SD logger disabled (no SD)");
    }

    // ALP SD logger — event-level CSV for drive data capture
    const bool alpLogEnabled = settingsManager.get().alpSdLogEnabled;
    alpSdLogger.setBootId(bootId, bootToken);
    alpSdLogger.begin(alpLogEnabled, sdEnabled, &gpsTimePublisher);
    if (alpSdLogger.isEnabled()) {
        SerialLog.printf("[ALP_SD] logger enabled (%s)\n", alpSdLogger.csvPath());
    }

    // GPS module
    {
        const V1Settings& gs = settingsManager.get();
        gpsRuntimeModule.begin(gs.gpsEnabled, gs.gpsEnablePinActiveHigh,
                               gs.gpsBaud,
                               &gpsTimePublisher, &gpsGeoPublisher);
        if (gs.gpsEnabled) {
            SerialLog.printf("[GPS] module enabled baud=%lu rx=%d tx=%d en=not-driven\n",
                             static_cast<unsigned long>(gs.gpsBaud),
                             1, 5);
        }
    }

    return bootId;
}

void initializeTouchAndDisplayControls() {
    // Initialize touch handler early - before BLE to avoid interleaved logs
    SerialLog.println("Initializing touch handler...");
    if (touchHandler.begin(17, 18, AXS_TOUCH_ADDR, -1)) {
        SerialLog.println("Touch handler initialized successfully");
    } else {
        SerialLog.println("[Touch] WARN: Touch handler failed to initialize - continuing anyway");
    }

    // Initialize BOOT button (GPIO 0) for brightness adjustment
    pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);
    const V1Settings& displaySettings = settingsManager.get();
    display.setBrightness(displaySettings.brightness);  // Apply saved brightness
    SerialLog.printf("[Settings] Applied saved brightness: %d\n",
                     displaySettings.brightness);
}

namespace {

void configureUiAutoPushModule(QuietCoordinatorModule& quietCoordinator) {
    // Initialize auto-push module after settings/profiles are ready
    autoPushModule.begin(&settingsManager,
                         &v1ProfileManager,
                         &bleClient,
                         &display,
                         &quietCoordinator);
}

void configureUiTouchInteractionModules(QuietCoordinatorModule& quietCoordinator) {
    configureTouchUiModule();

    tapGestureModule.begin(&touchHandler,
                           &settingsManager,
                           &display,
                           &bleClient,
                           &parser,
                           &autoPushModule,
                           &alertPersistenceModule,
                           &displayMode,
                           &quietCoordinator,
                           TapGestureModule::WifiCallbacks{
                               .isWifiActive = [](void*) { return wifiManager.isWifiServiceActive(); },
                               .stopWifi = [](void*) { wifiManager.stopSetupMode(true); },
                               .requestMaintenanceBoot = [](void*) {
                                   if (requestMaintenanceBoot()) {
                                       Serial.println("[MaintBoot] touch long-press requested maintenance reboot");
                                       settingsManager.save();
                                       markCleanShutdown();
                                       delay(50);
                                       ESP.restart();
                                   } else {
                                       Serial.println("[MaintBoot] ERROR: failed to persist maintenance request");
                                   }
                               },
                           });
}

}  // namespace

void configureUiInteractionModules(QuietCoordinatorModule& quietCoordinator) {
    configureUiAutoPushModule(quietCoordinator);
    configureUiTouchInteractionModules(quietCoordinator);
}

void logBootSummaryAndWifiStartup(uint32_t bootId, esp_reset_reason_t resetReason) {
    const V1Settings& bootSettings = settingsManager.get();
    const char* scenario = "default";
    extern const char* getBuildGitSha();
    const char* gitSha = getBuildGitSha();
    const char* resetStr = resetReasonToString(resetReason);
    SerialLog.printf("BOOT bootId=%lu reset=%s git=%s scenario=%s wifiMaster=%s\n",
                     static_cast<unsigned long>(bootId),
                     resetStr,
                     gitSha,
                     scenario,
                     bootSettings.enableWifi ? "on" : "off");

    if (!bootSettings.enableWifi) {
        SerialLog.println("[WiFi] Master disabled - startup and loop processing skipped");
    } else {
        SerialLog.println("[WiFi] Off in normal runtime - BOOT long-press reboots to maintenance");
    }
}

void initializeEarlyBootDiagnostics() {
    // Wait for USB to stabilize after upload.
    delay(50);

    // Release GPIO hold from deep-sleep fallback (backlight was held off).
    // Harmless no-op on normal power-on; must happen before display init.
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(static_cast<gpio_num_t>(LCD_BL));

    // Backlight is handled in display.begin() (inverted PWM for Waveshare).
    Serial.begin(115200);
    delay(30);  // Conservative USB CDC settle.

    // PANIC BREADCRUMBS: Log crash info FIRST (before any other init).
    logPanicBreadcrumbs();

    // Check NVS health early - before other subsystems start using it.
    nvsHealthCheck();
}
