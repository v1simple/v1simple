/**
 * Perf periodic report: task stack high-water logging and CSV snapshot emission.
 * Moved verbatim out of perf_metrics.cpp; no behavior change.
 */

#include "perf_metrics_internal.h"
#include "audio_beep.h"
#include "ble_bond_backup_writer.h"
#include "ble_client.h"
#include "display_drawn_region.h"
#include "perf_sd_logger.h"
#include "storage_manager.h"
#include "settings.h"
#include "main_globals.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/gps/gps_publishers.h"
#include "modules/system/system_event_bus.h"
#include "modules/wifi/wifi_auto_start_module.h"
#if PERF_METRICS && PERF_MONITORING && !defined(UNIT_TEST)
#include "modules/alp/alp_sd_logger.h"
#endif
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>

#if PERF_METRICS && PERF_MONITORING
namespace {
const char* stackLocationLabel(bool active, bool inPsram) {
    if (!active) {
        return "off";
    }
    return inPsram ? "psram" : "internal";
}
}  // namespace

static void reportTaskStackHighWaterMarks() {
#ifndef UNIT_TEST
    static uint32_t lastStackReportMs = 0;
    const uint32_t now = millis();
    constexpr uint32_t STACK_REPORT_INTERVAL_MS = 30000;
    if (lastStackReportMs != 0 && now - lastStackReportMs < STACK_REPORT_INTERVAL_MS) {
        return;
    }
    lastStackReportMs = now;

    const bool perfActive = perfSdLogger.writerTaskActive();
    const bool alpActive = alpSdLogger.writerTaskActive();
    const bool obdActive = obdRuntimeModule.transportTaskActive();
    const uint32_t audioPcmFreeBytes = audio_pcm_stack_high_water_bytes();
    const uint32_t audioSdFreeBytes = audio_sd_stack_high_water_bytes();
    const uint32_t discoveryFreeBytes = bleClient.discoveryTaskStackMinFreeBytes();
    const uint32_t bleBondFreeBytes =
        bleBondBackupWriterStats().writerStackMinFreeBytes;
    const bool audioPcmSampled = audioPcmFreeBytes != UINT32_MAX;
    const bool audioSdSampled = audioSdFreeBytes != UINT32_MAX;
    const bool discoverySampled = discoveryFreeBytes != UINT32_MAX;
    const bool bleBondSampled = bleBondFreeBytes != UINT32_MAX;
    if (!perfActive && !alpActive && !obdActive &&
        !audioPcmSampled && !audioSdSampled && !discoverySampled &&
        !bleBondSampled) {
        return;
    }

    Serial.printf("[STACK] aux_free_bytes perfSd=%lu/%s alpSd=%lu/%s obd=%lu/%s "
                  "audioPcm=%lu/%s audioSd=%lu/%s discovery=%lu/%s bleBond=%lu/%s\n",
                  static_cast<unsigned long>(
                      perfActive ? perfSdLogger.writerStackHighWaterBytes() : 0),
                  stackLocationLabel(perfActive, perfSdLogger.writerTaskStackInPsram()),
                  static_cast<unsigned long>(
                      alpActive ? alpSdLogger.writerStackHighWaterBytes() : 0),
                  stackLocationLabel(alpActive, alpSdLogger.writerTaskStackInPsram()),
                  static_cast<unsigned long>(
                      obdActive ? obdRuntimeModule.transportStackHighWaterBytes() : 0),
                  stackLocationLabel(obdActive, obdRuntimeModule.transportTaskStackInPsram()),
                  static_cast<unsigned long>(audioPcmSampled ? audioPcmFreeBytes : 0),
                  stackLocationLabel(audioPcmSampled, true),
                  static_cast<unsigned long>(audioSdSampled ? audioSdFreeBytes : 0),
                  stackLocationLabel(audioSdSampled, false),
                  static_cast<unsigned long>(discoverySampled ? discoveryFreeBytes : 0),
                  stackLocationLabel(discoverySampled, false),
                  static_cast<unsigned long>(bleBondSampled ? bleBondFreeBytes : 0),
                  stackLocationLabel(bleBondSampled, false));
#endif
}

bool perfMetricsCheckReport() {
    uint32_t now = millis();
    constexpr uint32_t STABILITY_REPORT_INTERVAL_MS = 5000;
    if (perfLastReportMs == 0) {
        perfLastReportMs = now;
        return false;
    }
    if (now - perfLastReportMs < STABILITY_REPORT_INTERVAL_MS) {
        return false;
    }
    perfLastReportMs = now;

    // Always capture the snapshot to cycle windowed maxima (prev-window
    // store + reset).  Without this, API-polled metrics like wifiMaxUs
    // accumulate as max-ever instead of per-window when SD is absent.
    PerfSdSnapshot snapshot{};
    captureSdSnapshot(snapshot);

    // Report stack high water marks for accessible tasks
    reportTaskStackHighWaterMarks();

    // Check DMA free threshold
    if (snapshot.freeDmaCap < 8192) {
        Serial.printf("[HEAP] WARNING: DMA free critically low: %lu bytes\n",
                      (unsigned long)snapshot.freeDmaCap);
    }

    if (perfSdLogger.isEnabled() && !perfMetricsIsSdCapturePaused()) {
        perfSdLogger.enqueue(snapshot);
    }
    return true;
}
#else
bool perfMetricsCheckReport() {
    return false;
}
#endif

bool perfMetricsEnqueueSnapshotNow() {
    if (!perfSdLogger.isEnabled() || perfMetricsIsSdCapturePaused()) {
        return false;
    }

    PerfSdSnapshot snapshot{};
    captureSdSnapshot(snapshot);
    return perfSdLogger.enqueue(snapshot);
}
