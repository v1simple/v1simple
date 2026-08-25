/**
 * Shared boot diagnostics and persistence lifecycle helpers.
 * Runtime-specific BLE, OBD, WiFi, touch, and display work belongs to the
 * owning DriveRuntime or MaintenanceRuntime.
 */

#include "main_internals.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_task_wdt.h>

#include "ble_bond_backup_writer.h"
#include "build_metadata.h"
#include "config.h"
#include "display_driver.h"
#include "modules/event_log/product_event_log.h"
#include "modules/health/health_journal.h"
#include "settings.h"
#include "settings_internals.h"

namespace {
void feedLoopTaskWatchdogDuringShutdown() {
    (void)esp_task_wdt_reset();
}
} // namespace

bool preparePersistenceForShutdown(ProductEventLog& events, HealthJournal& health, SettingsManager& settings) {
    feedLoopTaskWatchdogDuringShutdown();
    const bool eventWriterReleased = events.stopAndFlush(millis(), 750);
    if (!eventWriterReleased) {
        Serial.println("[Battery] WARN: product-event cleanup timed out; skipping competing SD writes");
    }
    feedLoopTaskWatchdogDuringShutdown();

    shutdownBleBondBackupWriter(1500);
    feedLoopTaskWatchdogDuringShutdown();

    if (eventWriterReleased) {
        Serial.println("[Battery] Saving settings...");
        settings.save();
        feedLoopTaskWatchdogDuringShutdown();
        Serial.println("[Battery] Forcing final SD settings backup...");
        settings.backupToSD();
        feedLoopTaskWatchdogDuringShutdown();
    }

    shutdownDeferredSettingsBackupWriter(1500);
    feedLoopTaskWatchdogDuringShutdown();
    if (eventWriterReleased) {
        health.end(millis());
        feedLoopTaskWatchdogDuringShutdown();
    }
    return eventWriterReleased;
}

bool completeLoggingForControlledRestart(ProductEventLog& events, HealthJournal& health) {
    if (!events.stopAndFlush(millis(), 750)) {
        Serial.println("[ProductEvents] WARN: cleanup timed out; restart continuing without competing SD writes");
        return false;
    }
    health.end(millis());
    return true;
}

void resumePersistenceAfterAbortedShutdown(ProductEventLog& events) {
    Serial.println("[Battery] Shutdown aborted; restoring persistence services...");
    markUncleanShutdown();
    if (events.enabled() && !events.resumeAfterAbortedShutdown(750)) {
        Serial.println("[ProductEvents] ERROR: writer admission could not be restored after shutdown abort");
    }
    resumeBleBondBackupWriterAfterAbortedShutdown();
    resumeDeferredSettingsBackupWriterAfterAbortedShutdown();
}

void logBootIdentity(uint32_t bootId, esp_reset_reason_t resetReason) {
    const V1Settings& bootSettings = settingsManager.get();
    Serial.printf("BOOT bootId=%lu uptimeMs=%lu reset=%s git=%s image=%s wifiMaster=%s\n",
                  static_cast<unsigned long>(bootId), static_cast<unsigned long>(millis()),
                  resetReasonToString(resetReason), getBuildGitSha(), getRuntimeImageId(),
                  bootSettings.enableWifi ? "on" : "off");
}

void initializeEarlyBootDiagnostics() {
    delay(50);
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(static_cast<gpio_num_t>(LCD_BL));
    digitalWrite(LCD_BL, HIGH);

    Serial.begin(115200);
    delay(30);
    logPanicBreadcrumbs();
    nvsHealthCheck();
}
