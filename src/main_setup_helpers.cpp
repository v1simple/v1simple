/**
 * Setup and shutdown orchestration helpers.
 */

#include "main_internals.h"
#include "main_globals.h"

#include <Arduino.h>
#include <algorithm>
#include <esp_task_wdt.h>

#include "config.h"
#include "audio_beep.h"
#include "battery_manager.h"
#include "ble_bond_backup_writer.h"
#include "ble_client.h"
#include "build_metadata.h"
#include "display.h"
#include "display_mode.h"
#include "packet_parser.h"
#include "modules/gps/gps_runtime_module.h"
#include "settings.h"
#include "settings_internals.h"
#include "settings_runtime_sync.h"
#include "storage_manager.h"
#include "touch_handler.h"
#include "v1_devices.h"
#include "v1_profiles.h"
#include "wifi_manager.h"
#include "modules/auto_push/auto_push_module.h"
#include "modules/ble/connection_state_module.h"
#include "modules/alert_persistence/alert_persistence_module.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/event_log/product_event_log.h"
#include "modules/health/health_journal.h"
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
    bool connectedAddressFromLink = false;
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
        selection.connectedAddressFromLink = selection.connectedAddress.length() > 0;
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

} // namespace

void prepareForShutdown(void* /*context*/) {
    feedLoopTaskWatchdogDuringShutdown();

    const bool eventWriterReleased = productEventLog.stopAndFlush(millis(), 750);
    if (!eventWriterReleased) {
        Serial.println("[Battery] WARN: product-event cleanup timed out; skipping competing SD writes");
    }
    feedLoopTaskWatchdogDuringShutdown();

    // ── BLE teardown ─────────────────────────────────────────────────
    // Drain the best-effort bond snapshot before the final synchronous
    // settings write takes exclusive ownership of SD.
    shutdownBleBondBackupWriter(1500);
    feedLoopTaskWatchdogDuringShutdown();

    if (eventWriterReleased) {
        Serial.println("[Battery] Saving settings...");
        settingsManager.save();
        feedLoopTaskWatchdogDuringShutdown();

        Serial.println("[Battery] Forcing final SD settings backup...");
        settingsManager.backupToSD();
        feedLoopTaskWatchdogDuringShutdown();
    }

    // Stop the deferred-backup writer task now that the final synchronous
    // backup has landed. Without this signal, the writer keeps polling and
    // would emit failed-write logs once the SD is torn down.
    shutdownDeferredSettingsBackupWriter(1500);
    feedLoopTaskWatchdogDuringShutdown();

    if (eventWriterReleased) {
        healthJournal.end(millis());
        feedLoopTaskWatchdogDuringShutdown();
    }

    // MaintenanceRuntime owns WiFi teardown. Normal runtime owns BLE/OBD.
    // Each runtime restores only the services that it constructed if the
    // hardware shutdown tail returns.
    if (!mainRuntimeState.maintenanceBootActive) {
        Serial.println("[Battery] Disconnecting BLE peripherals before shutdown...");
        bleClient.disconnect();
        if (!obdRuntimeModule.disconnectForShutdown(100)) {
            Serial.println("[Battery] WARN: OBD transport disconnect did not acknowledge before shutdown");
        }
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (pScan && pScan->isScanning()) {
            pScan->stop();
        }
        delay(50);
        feedLoopTaskWatchdogDuringShutdown();
    }

    // Mark this shutdown as clean LAST.  If anything below fails or the
    // user yanks power mid-teardown we want the next boot to still see
    // "unclean" and record the integrity event.
    if (eventWriterReleased) {
        Serial.println("[Battery] Writing clean-shutdown marker...");
        markCleanShutdown();
    }
}

bool completeLoggingForControlledRestart() {
    if (!productEventLog.stopAndFlush(millis(), 750)) {
        Serial.println("[ProductEvents] WARN: cleanup timed out; restart continuing without competing SD writes");
        return false;
    }
    healthJournal.end(millis());
    return true;
}

void resumeAfterAbortedShutdown(void* /*context*/) {
    Serial.println("[Battery] Shutdown aborted; restoring persistence services...");

    // Correct the boot-integrity marker first. If power disappears again while
    // recovery is still running, the next boot must classify this run as unclean.
    markUncleanShutdown();

    if (productEventLog.enabled() && !productEventLog.resumeAfterAbortedShutdown(750)) {
        Serial.println("[ProductEvents] ERROR: writer admission could not be restored after shutdown abort");
    }

    // Reopen the older persistence writers after the product-event owner has
    // either reused its live task or restarted exactly one confirmed exit.
    resumeBleBondBackupWriterAfterAbortedShutdown();
    resumeDeferredSettingsBackupWriterAfterAbortedShutdown();

    if (!mainRuntimeState.maintenanceBootActive) {
        // disconnect() quiesces asynchronously. This starts immediately when
        // quiescence is complete; otherwise normal process() resumes scanning.
        bleClient.startScanning();
    }
}

void onV1ConnectImmediate() {
    mainRuntimeState.v1ConnectedAtMs = millis();
    connectionStateModule.handleConnected(mainRuntimeState.v1ConnectedAtMs, bleClient.sessionGeneration());
    productEventLog.observeV1Link(true, static_cast<uint32_t>(mainRuntimeState.v1ConnectedAtMs));
}

void onV1SessionOpened(uint32_t sessionGeneration) {
    connectionStateModule.handleSessionOpened(sessionGeneration);
}

void onV1SessionClosed(uint32_t sessionGeneration) {
    const uint32_t nowMs = millis();
    productEventLog.observeV1Link(false, nowMs);
    connectionStateModule.handleSessionClosed(nowMs, sessionGeneration);
}

void onV1Connected() {
    const V1Settings& s = settingsManager.get();
    const V1ConnectedAutoPushSelection selection = resolveV1ConnectedAutoPushSelection(s);

    if (selection.connectedAddressFromLink) {
        settingsManager.setLastV1Address(selection.connectedAddress);
        if (!v1DeviceStore.isReady()) {
            settingsManager.requestLastV1AddressFallbackPersist(selection.connectedAddress);
        }
    }

    const AutoPushSlot& slot = settingsManager.getSlot(selection.selectedSlotIndex);
    Serial.printf("[AutoPush] onV1Connected autoPush=%s activeSlot=%d selectedSlot=%d defaultProfile=%u mode=%d\n",
                     s.autoPushEnabled ? "on" : "off", selection.activeSlotIndex, selection.selectedSlotIndex,
                     static_cast<unsigned>(selection.deviceDefaultProfile), static_cast<int>(slot.mode));
    if (!s.autoPushEnabled) {
        return;
    }

    display.setProfileIndicatorSlot(selection.selectedSlotIndex);
    const auto queueResult = autoPushModule.queueSlotPush(selection.selectedSlotIndex, false, false);
    (void)queueResult;
}

void initializeStorageAndProfiles() {
    // Mount storage (SD if available, else LittleFS) for profiles and settings.
    Serial.println("[Setup] Mounting storage...");
    if (storageManager.begin()) {
        Serial.printf("[Setup] Storage ready: %s\n", storageManager.statusText().c_str());
        v1ProfileManager.begin(storageManager.getFilesystem(), storageManager.getLittleFS());
        v1DeviceStore.begin(storageManager.getFilesystem(), storageManager.getLittleFS());
        if (!mainRuntimeState.maintenanceBootActive) {
            audio_init_buffers();
            audio_init_sd();
        } else {
            Serial.println("[Setup] Maintenance boot: skipping audio buffer/voice init");
        }

        // Retry settings restore now that SD is mounted
        // (settings.begin() runs before storage, so restore may have failed)
        if (settingsManager.checkAndRestoreFromSD()) {
            // Settings were restored from SD - update display with restored brightness.
            display.setBrightness(settingsManager.get().brightness);
        }

        const String storedFallback = settingsManager.loadLastV1AddressFallback();
        const String degradedFallback = normalizeV1DeviceAddress(storedFallback);
        if (storedFallback.length() > 0 && degradedFallback.length() == 0) {
            settingsManager.clearLastV1AddressFallback();
        }

        const String settingsFallback = normalizeV1DeviceAddress(settingsManager.get().lastV1Address);
        const String restoredLastKnownV1 = degradedFallback.length() > 0 ? degradedFallback : settingsFallback;
        if (restoredLastKnownV1.length() > 0) {
            settingsManager.setLastV1Address(restoredLastKnownV1);
            if (v1DeviceStore.isReady() && v1DeviceStore.upsertDevice(restoredLastKnownV1) &&
                degradedFallback.length() > 0) {
                settingsManager.clearLastV1AddressFallback();
            }
        }

        // Validate profile references in auto-push slots.
        // Clear references to profiles that don't exist.
        settingsManager.validateProfileReferences(v1ProfileManager);
    } else {
        Serial.println("[Setup] Storage unavailable - profiles will be disabled");
        const String storedFallback = settingsManager.loadLastV1AddressFallback();
        const String degradedFallback = normalizeV1DeviceAddress(storedFallback);
        if (degradedFallback.length() > 0) {
            settingsManager.setLastV1Address(degradedFallback);
            Serial.println("[Setup] Restored degraded V1 address fallback from NVS");
        } else if (storedFallback.length() > 0) {
            settingsManager.clearLastV1AddressFallback();
        }
    }

    const V1Settings& gpsSettings = settingsManager.get();
    gpsRuntimeModule.begin(gpsSettings.gpsEnabled, gpsSettings.gpsBaud);
    if (gpsSettings.gpsEnabled) {
        Serial.printf("[GPS] module enabled baud=%lu rx=%d tx=%d en=not-driven\n",
                      static_cast<unsigned long>(gpsSettings.gpsBaud), 1, 5);
    }
}

void initializeTouchAndDisplayControls() {
    // Initialize touch handler early - before BLE to avoid interleaved logs
    Serial.println("Initializing touch handler...");
    if (touchHandler.begin(17, 18, AXS_TOUCH_ADDR, -1)) {
        Serial.println("Touch handler initialized successfully");
    } else {
        Serial.println("[Touch] WARN: Touch handler failed to initialize - continuing anyway");
    }

    // Initialize BOOT button (GPIO 0) for brightness adjustment
    pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);
    const V1Settings& displaySettings = settingsManager.get();
    display.setBrightness(displaySettings.brightness); // Apply saved brightness
    Serial.printf("[Settings] Applied saved brightness: %d\n", displaySettings.brightness);
}

namespace {

void configureUiAutoPushModule(QuietCoordinatorModule& quietCoordinator) {
    // Initialize auto-push module after settings/profiles are ready
    autoPushModule.begin(&settingsManager, &v1ProfileManager, &bleClient, &display, &quietCoordinator);
}

void configureUiTouchInteractionModules(QuietCoordinatorModule& quietCoordinator) {
    configureTouchUiModule();

    tapGestureModule.begin(&touchHandler, &settingsManager, &display, &bleClient, &parser, &autoPushModule,
                           &alertPersistenceModule, &displayMode, &quietCoordinator,
                           TapGestureModule::WifiCallbacks{
                               .isWifiActive = [](void*) { return wifiManager.isWifiServiceActive(); },
                               .stopWifi = [](void*) { wifiManager.stopSetupMode(true); },
                               .requestMaintenanceBoot =
                                   [](void*) {
                                       if (requestMaintenanceBoot()) {
                                           Serial.println("[MaintBoot] touch long-press requested maintenance reboot");
                                           const bool persistenceSafe = completeLoggingForControlledRestart();
                                           if (persistenceSafe) {
                                               settingsManager.save();
                                               markCleanShutdown();
                                           } else {
                                               Serial.println("[MaintBoot] WARN: touch restart continuing without final persistence writes");
                                           }
                                           delay(50);
                                           ESP.restart();
                                       } else {
                                           Serial.println("[MaintBoot] ERROR: failed to persist maintenance request");
                                       }
                                   },
                           });
}

} // namespace

void configureUiInteractionModules(QuietCoordinatorModule& quietCoordinator) {
    configureUiAutoPushModule(quietCoordinator);
    configureUiTouchInteractionModules(quietCoordinator);
}

void logBootIdentity(uint32_t bootId, esp_reset_reason_t resetReason) {
    const V1Settings& bootSettings = settingsManager.get();
    const char* gitSha = getBuildGitSha();
    const char* imageId = getRuntimeImageId();
    const char* resetStr = resetReasonToString(resetReason);
    Serial.printf("BOOT bootId=%lu uptimeMs=%lu reset=%s git=%s image=%s wifiMaster=%s\n",
                  static_cast<unsigned long>(bootId), static_cast<unsigned long>(millis()), resetStr, gitSha,
                  imageId, bootSettings.enableWifi ? "on" : "off");
}

void logBootSummaryAndWifiStartup(uint32_t bootId, esp_reset_reason_t resetReason) {
    logBootIdentity(bootId, resetReason);
    const V1Settings& bootSettings = settingsManager.get();
    if (!bootSettings.enableWifi) {
        Serial.println("[WiFi] Master disabled - startup and loop processing skipped");
    } else {
        Serial.println("[WiFi] Off in normal runtime - BOOT long-press reboots to maintenance");
    }
}

void initializeEarlyBootDiagnostics() {
    // Wait for USB to stabilize after upload.
    delay(50);

    // Release the deep-sleep fallback hold without exposing the panel's retained
    // GRAM. Preload the inverted backlight output HIGH (off), release the hold,
    // then assert HIGH again before any slower battery/display initialization.
    // Harmless on normal power-on; required to prevent a stale GOODBYE frame
    // from flashing during a button wake.
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(static_cast<gpio_num_t>(LCD_BL));
    digitalWrite(LCD_BL, HIGH);

    // Backlight is handled in display.begin() (inverted PWM for Waveshare).
    Serial.begin(115200);
    delay(30); // Conservative USB CDC settle.

    // PANIC BREADCRUMBS: Log crash info FIRST (before any other init).
    logPanicBreadcrumbs();

    // Check NVS health early - before other subsystems start using it.
    nvsHealthCheck();
}
