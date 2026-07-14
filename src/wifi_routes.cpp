/**
 * WiFi Routes — setupWebServer() route registrations.
 * Extracted from wifi_manager.cpp for maintainability.
 */

#include "wifi_manager_internals.h"
#include "main_globals.h"
#include "perf_metrics.h"
#include "settings.h"
#include "littlefs_mount.h"
#include "storage_manager.h"
#include "modules/wifi/backup_api_service.h"
#include "modules/wifi/wifi_quiet_api_service.h"
#include "modules/wifi/wifi_client_api_service.h"
#include "modules/wifi/wifi_display_colors_api_service.h"
#include "modules/wifi/wifi_display_visual_api_service.h"
#include "modules/wifi/wifi_portal_api_service.h"
#include "modules/wifi/wifi_settings_api_service.h"
#include "modules/wifi/wifi_status_api_service.h"
#include "modules/wifi/wifi_autopush_api_service.h"
#include "modules/wifi/wifi_audio_api_service.h"
#include "modules/wifi/wifi_maintenance_write_policy.h"
#include "modules/wifi/wifi_static_path_guard.h"
#include "modules/wifi/wifi_v1_profile_api_service.h"
#include "modules/wifi/wifi_v1_devices_api_service.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/obd/obd_api_service.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/alp/alp_api_service.h"
#include "modules/alp/alp_runtime_module.h"
#include "modules/gps/gps_api_service.h"
#include "modules/gps/gps_runtime_module.h"
#include "battery_manager.h"
#include <LittleFS.h>

bool WiFiManager::requireMaintenanceApiWriteHeader() {
    const bool hasValidWriteHeader =
        server_.hasHeader(maintenanceApiWriteHeader()) &&
        server_.header(maintenanceApiWriteHeader()) == maintenanceApiWriteHeaderValue();
    const WifiMaintenanceWritePolicy::Decision decision =
        WifiMaintenanceWritePolicy::evaluate(maintenanceBootMode_, hasValidWriteHeader);

    switch (decision) {
        case WifiMaintenanceWritePolicy::Decision::Allow:
            return true;
        case WifiMaintenanceWritePolicy::Decision::RejectNotMaintenance:
            Serial.printf("[HTTP] REJECT maintenance write outside maintenance boot %s\n",
                          server_.uri().c_str());
            break;
        case WifiMaintenanceWritePolicy::Decision::RejectHeader:
            Serial.printf("[HTTP] REJECT invalid maintenance write header %s\n",
                          server_.uri().c_str());
            break;
    }

    server_.send(403, "application/json", "{\"success\":false,\"error\":\"forbidden\"}");
    return false;
}

bool WiFiManager::setupWebServer() {
    // Initialize LittleFS for serving web UI files
    if (!fsmount::mountStorage()) {
        return false;
    }

    // WebServer::stop() only closes the listening socket; registered handlers
    // persist on the server instance across WiFi restarts.
    if (webRoutesInitialized_) {
        return true;
    }

    // New UI served from LittleFS
    // Serve static assets from _app directory
    server_.on("/_app/env.js", HTTP_GET, [this]() {
        if (!serveLittleFSFile("/_app/env.js", "application/javascript")) {
            server_.send(404, "text/plain", "Not found");
        }
    });
    server_.on("/_app/version.json", HTTP_GET, [this]() {
        if (!serveLittleFSFile("/_app/version.json", "application/json")) {
            server_.send(404, "text/plain", "Not found");
        }
    });

    // Root serves /index.html (Svelte app)
    server_.on("/", HTTP_GET, [this]() {
        markUiActivity();  // Track UI activity
        if (serveLittleFSFile("/index.html", "text/html")) {
            return;
        }
        // LittleFS missing - tell user to reflash
        Serial.println("[HTTP] 500 / -> LittleFS missing");
        server_.send(500, "application/json", "{\"success\":false,\"error\":\"Web UI not found. Please reflash with ./build.sh --all\"}");
    });

    // Catch-all for _app/immutable/* files (if Svelte files are uploaded)
    server_.onNotFound([this]() {
        markUiActivity();  // Track UI activity
        String uri = server_.uri();

        if (!WifiStaticPathGuard::isAllowedServedPath(uri.c_str())) {
            if (!WifiStaticPathGuard::isSafe(uri.c_str())) {
                Serial.printf("[HTTP] REJECT unsafe path %s\n", uri.c_str());
            } else {
                Serial.printf("[HTTP] REJECT unlisted static path %s\n", uri.c_str());
            }
            server_.send(404, "application/json", "{\"success\":false,\"error\":\"Not found\"}");
            return;
        }

        // Serve _app files from LittleFS
        if (uri.startsWith("/_app/")) {
            String contentType = "application/octet-stream";
            if (uri.endsWith(".js")) contentType = "application/javascript";
            else if (uri.endsWith(".css")) contentType = "text/css";
            else if (uri.endsWith(".json")) contentType = "application/json";

            if (serveLittleFSFile(uri.c_str(), contentType.c_str())) {
                return;
            }
        }

        // Fall through to original not found handler
        handleNotFound();
    });

    // New API endpoints (PHASE A)
    server_.on("/api/status", HTTP_GET, [this]() {
        WifiStatusApiService::handleApiStatus(
            server_,
            makeStatusRuntime(),
            cachedStatusJson_,
            lastStatusJsonTime_,
            STATUS_CACHE_TTL_MS,
            [](void* /*ctx*/) -> unsigned long { return millis(); }, nullptr,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });

    server_.on("/api/device/settings", HTTP_GET, [this]() {
        WifiSettingsApiService::handleApiDeviceSettingsGet(server_, makeSettingsRuntime());
    });
    server_.on("/api/device/settings", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiSettingsApiService::handleApiDeviceSettingsSave(server_, makeSettingsRuntime());
    });

    // Lightweight health and captive-portal helpers
    server_.on("/ping", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiPing(
            server_,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); },
            this);
    });
    // Android/ChromeOS captive portal probes
    server_.on("/generate_204", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiGenerate204(
            server_,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); },
            this);
    });
    server_.on("/gen_204", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiGen204(
            server_,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); },
            this);
    });
    // iOS/macOS captive portal
    server_.on("/hotspot-detect.html", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiHotspotDetect(
            server_,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); },
            this);
    });
    // Windows captive portal variants
    server_.on("/fwlink", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiFwlink(server_);
    });
    server_.on("/ncsi.txt", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiNcsiTxt(server_);
    });

    // V1 Settings/Profiles routes
    server_.on("/api/v1/profiles", HTTP_GET, [this]() {
        WifiV1ProfileApiService::handleApiProfilesList(server_, makeV1ProfileRuntime());
    });
    server_.on("/api/v1/profile", HTTP_GET, [this]() {
        WifiV1ProfileApiService::handleApiProfileGet(server_, makeV1ProfileRuntime());
    });
    server_.on("/api/v1/profile", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiV1ProfileApiService::handleApiProfileSave(
            server_,
            makeV1ProfileRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/v1/profile/delete", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiV1ProfileApiService::handleApiProfileDelete(
            server_,
            makeV1ProfileRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/v1/pull", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiV1ProfileApiService::handleApiSettingsPull(
            server_,
            makeV1ProfileRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/v1/push", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiV1ProfileApiService::handleApiSettingsPush(
            server_,
            makeV1ProfileRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/v1/current", HTTP_GET, [this]() {
        WifiV1ProfileApiService::handleApiCurrentSettings(server_, makeV1ProfileRuntime());
    });
    server_.on("/api/v1/devices", HTTP_GET, [this]() {
        WifiV1DevicesApiService::handleApiDevicesList(server_, makeV1DevicesRuntime());
    });
    server_.on("/api/v1/devices/name", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiV1DevicesApiService::handleApiDeviceNameSave(
            server_,
            makeV1DevicesRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/v1/devices/profile", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiV1DevicesApiService::handleApiDeviceProfileSave(
            server_,
            makeV1DevicesRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/v1/devices/delete", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiV1DevicesApiService::handleApiDeviceDelete(
            server_,
            makeV1DevicesRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });

    // Auto-Push routes
    server_.on("/api/autopush/slots", HTTP_GET, [this]() {
        WifiAutoPushApiService::handleApiSlots(server_, makeAutoPushRuntime());
    });
    server_.on("/api/autopush/slot", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiAutoPushApiService::handleApiSlotSave(
            server_,
            makeAutoPushRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/autopush/activate", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiAutoPushApiService::handleApiActivate(
            server_,
            makeAutoPushRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/autopush/push", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiAutoPushApiService::handleApiPushNow(
            server_,
            makeAutoPushRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/autopush/status", HTTP_GET, [this]() {
        WifiAutoPushApiService::handleApiStatus(server_, makeAutoPushRuntime());
    });

    // Display settings routes
    server_.on("/api/display/settings", HTTP_GET, [this]() {
        WifiDisplayColorsApiService::handleApiGet(server_, makeDisplayColorsRuntime());
    });
    server_.on("/api/display/settings", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiDisplayColorsApiService::handleApiSave(
            server_,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/display/settings/reset", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiDisplayColorsApiService::handleApiReset(
            server_,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/display/preview", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiDisplayColorsApiService::handleApiPreview(
            server_,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/display/preview/clear", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiDisplayColorsApiService::handleApiClear(
            server_,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/display/visual/steps", HTTP_GET, [this]() {
        WifiDisplayVisualApiService::handleSteps(server_, makeDisplayVisualRuntime());
    });
    server_.on("/api/display/visual/layout", HTTP_GET, [this]() {
        WifiDisplayVisualApiService::handleLayout(server_, makeDisplayVisualRuntime());
    });
    server_.on("/api/display/visual/pin", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiDisplayVisualApiService::handlePin(
            server_,
            makeDisplayVisualRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/display/visual/framebuffer", HTTP_GET, [this]() {
        WifiDisplayVisualApiService::handleFramebuffer(server_, makeDisplayVisualRuntime());
    });
    server_.on("/api/display/visual/flushshadow", HTTP_GET, [this]() {
        WifiDisplayVisualApiService::handleFlushShadow(server_, makeDisplayVisualRuntime());
    });
    server_.on("/api/display/visual/clear", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiDisplayVisualApiService::handleClear(server_, makeDisplayVisualRuntime());
    });

    // Audio settings routes
    server_.on("/api/audio/settings", HTTP_GET, [this]() {
        WifiAudioApiService::handleApiGet(server_, makeAudioRuntime());
    });
    server_.on("/api/audio/settings", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiAudioApiService::handleApiSave(server_, makeAudioRuntime());
    });

    // Quiet-driving settings routes
    server_.on("/api/quiet/settings", HTTP_GET, [this]() {
        WifiQuietApiService::handleApiGet(server_, makeQuietRuntime());
    });
    server_.on("/api/quiet/settings", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiQuietApiService::handleApiSave(server_, makeQuietRuntime());
    });

    // Settings backup/restore API routes
    server_.on("/api/settings/backup", HTTP_GET, [this]() {
        BackupApiService::handleApiBackup(
            server_,
            cachedBackupSnapshot_,
            makeBackupRuntime(),
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this,
            [](void* /*ctx*/) { return static_cast<uint32_t>(millis()); }, nullptr);
    });
    server_.on("/api/settings/backup-now", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        BackupApiService::handleApiBackupNow(
            server_,
            makeBackupRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/settings/restore", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        BackupApiService::handleApiRestore(
            server_,
            makeBackupRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });

    // WiFi client (STA) API routes - connect to external network
    server_.on("/api/wifi/status", HTTP_GET, [this]() {
        WifiClientApiService::handleApiStatus(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/wifi/scan", HTTP_GET, [this]() {
        WifiClientApiService::handleApiScanStatus(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/wifi/scan", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiClientApiService::handleApiScan(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/wifi/disconnect", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiClientApiService::handleApiDisconnect(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/wifi/forget", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiClientApiService::handleApiForget(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/wifi/enable", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiClientApiService::handleApiEnable(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/wifi/networks", HTTP_GET, [this]() {
        WifiClientApiService::handleApiNetworks(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/wifi/networks", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiClientApiService::handleApiNetworksSave(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/wifi/networks/delete", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiClientApiService::handleApiNetworksDelete(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/wifi/networks/test", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        WifiClientApiService::handleApiNetworksTest(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    // OBD API routes
    server_.on("/api/obd/status", HTTP_GET, [this]() {
        ObdApiService::handleApiStatus(server_, *obdRuntime_, makeObdRuntime());
    });
    server_.on("/api/obd/devices", HTTP_GET, [this]() {
        ObdApiService::handleApiDevicesList(server_, *obdRuntime_, settingsManager, makeObdRuntime());
    });
    server_.on("/api/obd/config", HTTP_GET, [this]() {
        ObdApiService::handleApiConfigGet(server_, settingsManager, makeObdRuntime());
    });
    server_.on("/api/obd/devices/name", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        ObdApiService::handleApiDeviceNameSave(server_, settingsManager, makeObdRuntime());
    });
    server_.on("/api/obd/scan", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        ObdApiService::handleApiScan(server_, *obdRuntime_, makeObdRuntime());
    });
    server_.on("/api/obd/forget", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        ObdApiService::handleApiForget(server_, *obdRuntime_, settingsManager, makeObdRuntime());
    });
    server_.on("/api/obd/config", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        ObdApiService::handleApiConfig(server_,
                                       *obdRuntime_,
                                       settingsManager,
                                       makeObdRuntime());
    });

    // ALP API routes — runtime status snapshot for diagnostics/UI
    server_.on("/api/alp/status", HTTP_GET, [this]() {
        if (!alpRuntime_) {
            server_.send(503, "application/json",
                         "{\"error\":\"alp runtime not wired\"}");
            return;
        }
        AlpApiService::handleApiStatus(server_, *alpRuntime_,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this,
            mainRuntimeState.maintenanceBootActive);
    });

    // GPS API routes — config + status
    server_.on("/api/gps/config", HTTP_GET, [this]() {
        GpsApiService::Runtime r;
        r.ctx = this;
        r.markUiActivity = [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); };
        GpsApiService::handleApiConfigGet(server_, settingsManager, r);
    });
    server_.on("/api/gps/config", HTTP_POST, [this]() {
        if (!requireMaintenanceApiWriteHeader()) return;
        GpsApiService::Runtime r;
        r.ctx = this;
        r.markUiActivity = [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); };
        if (!gpsRuntime_) {
            server_.send(503, "application/json",
                         "{\"error\":\"gps runtime not wired\"}");
            return;
        }
        GpsApiService::handleApiConfigSave(server_, settingsManager, *gpsRuntime_, r);
    });
    server_.on("/api/gps/status", HTTP_GET, [this]() {
        if (!gpsRuntime_) {
            server_.send(503, "application/json",
                         "{\"error\":\"gps runtime not wired\"}");
            return;
        }
        GpsApiService::Runtime r;
        r.ctx = this;
        r.markUiActivity = [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); };
        r.maintenanceBootActive = mainRuntimeState.maintenanceBootActive;
        GpsApiService::handleApiStatus(server_, *gpsRuntime_, r);
    });


    // Note: onNotFound is set earlier to handle LittleFS static files


    webRoutesInitialized_ = true;
    return true;
}
