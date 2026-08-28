/**
 * WiFi HTTP route registration.
 */

#include "wifi_manager_internals.h"
#include "settings.h"
#include "littlefs_mount.h"
#include "storage_manager.h"
#include "modules/wifi/backup_api_service.h"
#include "modules/wifi/wifi_system_api_service.h"
#include "modules/wifi/wifi_quiet_api_service.h"
#include "modules/wifi/wifi_client_api_service.h"
#include "modules/wifi/wifi_display_colors_api_service.h"
#include "modules/wifi/wifi_portal_api_service.h"
#include "modules/wifi/wifi_settings_api_service.h"
#include "modules/wifi/wifi_status_api_service.h"
#include "modules/wifi/wifi_autopush_api_service.h"
#include "modules/wifi/wifi_audio_api_service.h"
#include "modules/wifi/wifi_maintenance_write_policy.h"
#include "modules/wifi/wifi_split_boot_api_response.h"
#include "modules/wifi/wifi_static_path_guard.h"
#include "modules/wifi/wifi_v1_profile_api_service.h"
#include "modules/wifi/wifi_v1_devices_api_service.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/obd/obd_api_service.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/gps/gps_api_service.h"
#include "modules/gps/gps_runtime_module.h"
#include "battery_manager.h"
#include "main_internals.h"
#include <LittleFS.h>

bool WiFiManager::hasMaintenanceWriteRequestShape() const {
    return maintenanceBootMode_ && server_.hasHeader(maintenanceApiWriteHeader()) &&
           server_.header(maintenanceApiWriteHeader()) == maintenanceApiWriteHeaderValue();
}

bool WiFiManager::requireMaintenanceWriteRequestShape() {
    const bool hasValidWriteHeader = hasMaintenanceWriteRequestShape();
    const WifiMaintenanceWritePolicy::Decision decision =
        WifiMaintenanceWritePolicy::evaluate(maintenanceBootMode_, hasValidWriteHeader);

    switch (decision) {
    case WifiMaintenanceWritePolicy::Decision::Allow:
        return true;
    case WifiMaintenanceWritePolicy::Decision::RejectNotMaintenance:
        Serial.printf("[HTTP] REJECT maintenance write outside maintenance boot %s\n", server_.uri().c_str());
        break;
    case WifiMaintenanceWritePolicy::Decision::RejectHeader:
        Serial.printf("[HTTP] REJECT invalid maintenance write request shape %s\n", server_.uri().c_str());
        break;
    }

    server_.send(403, "application/json", "{\"success\":false,\"error\":\"forbidden\"}");
    return false;
}

void WiFiManager::registerMaintenanceWriteRoute(const char* uri, WebServer::THandlerFunction handler) {
    // WifiMaintenanceWebServer admits bounded writes before _parseRequest;
    // accepted requests then use the framework's normal POST parsing path.
    server_.on(uri, HTTP_POST, [this, handler = std::move(handler)]() mutable {
        WifiMaintenanceWritePolicy::dispatchStorageResolved(
            server_, maintenanceWritePreAdmitted_,
            [this]() { return settings_.resolveStorageTransactionsForMutation(); }, handler);
    });
}

bool WiFiManager::setupWebServer() {
    // Cache the active AP address before starting the listener. Accepted
    // sockets for any other local destination are closed before request
    // parsing; allowed AP sockets then pass through the bounded preflight.
    server_.setMaintenanceApIp(WiFi.softAPIP());
    // Consult STA's address at acceptance time. If DHCP assigns the AP's
    // address, localIP alone cannot distinguish interfaces and ingress fails
    // closed until the lifecycle disconnects that STA.
    server_.setLiveStaIp([]() {
        return WiFi.status() == WL_CONNECTED ? static_cast<uint32_t>(WiFi.localIP()) : 0U;
    });
    server_.setMaintenanceBootMode(maintenanceBootMode_);
    server_.setWriteAdmission([this]() { return admitMaintenanceWriteBeforeBody(); });

    // Initialize LittleFS for serving web UI files
    if (!fsmount::mountStorage()) {
        return false;
    }

    // WebServer::stop() only closes the listening socket; registered handlers
    // persist on the server instance across WiFi restarts.
    if (webRoutesInitialized_) {
        return true;
    }

    // Serve the LittleFS UI assets from the _app directory.
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
        markUiActivity(); // Track UI activity
        if (serveLittleFSFile("/index.html", "text/html")) {
            return;
        }
        // LittleFS missing - tell user to reflash
        Serial.println("[HTTP] 500 / -> LittleFS missing");
        server_.send(500, "application/json",
                     "{\"success\":false,\"error\":\"Web UI not found. Please reflash with ./build.sh --all\"}");
    });

    // Catch-all for _app/immutable/* files (if Svelte files are uploaded)
    server_.onNotFound([this]() {
        markUiActivity(); // Track UI activity
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
            if (uri.endsWith(".js"))
                contentType = "application/javascript";
            else if (uri.endsWith(".css"))
                contentType = "text/css";
            else if (uri.endsWith(".json"))
                contentType = "application/json";

            if (serveLittleFSFile(uri.c_str(), contentType.c_str())) {
                return;
            }
        }

        // Fall through to original not found handler
        handleNotFound();
    });

    // Device API routes.
    server_.on("/api/status", HTTP_GET, [this]() {
        WifiStatusApiService::handleApiStatus(
            server_, makeStatusRuntime(), cachedStatusJson_, lastStatusJsonTime_, STATUS_CACHE_TTL_MS,
            [](void* /*ctx*/) -> unsigned long { return millis(); }, nullptr,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });

    server_.on("/api/device/settings", HTTP_GET,
               [this]() { WifiSettingsApiService::handleApiDeviceSettingsGet(server_, makeSettingsRuntime()); });
    registerMaintenanceWriteRoute("/api/device/settings", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiSettingsApiService::handleApiDeviceSettingsSave(server_, makeSettingsRuntime());
    });

    // Lightweight health and captive-portal helpers
    server_.on("/ping", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiPing(
            server_, [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    // Android/ChromeOS captive portal probes
    server_.on("/generate_204", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiGenerate204(
            server_, [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/gen_204", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiGen204(
            server_, [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    // iOS/macOS captive portal
    server_.on("/hotspot-detect.html", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiHotspotDetect(
            server_, [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    // Windows captive portal variants
    server_.on("/fwlink", HTTP_GET, [this]() { WifiPortalApiService::handleApiFwlink(server_); });
    server_.on("/ncsi.txt", HTTP_GET, [this]() { WifiPortalApiService::handleApiNcsiTxt(server_); });

    // V1 Settings/Profiles routes
    server_.on("/api/v1/profiles", HTTP_GET,
               [this]() { WifiV1ProfileApiService::handleApiProfilesList(server_, makeV1ProfileRuntime()); });
    server_.on("/api/v1/profile", HTTP_GET,
               [this]() { WifiV1ProfileApiService::handleApiProfileGet(server_, makeV1ProfileRuntime()); });
    registerMaintenanceWriteRoute("/api/v1/profile", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiV1ProfileApiService::handleApiProfileSave(
            server_, makeV1ProfileRuntime(), [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });
    registerMaintenanceWriteRoute("/api/v1/profile/delete", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiV1ProfileApiService::handleApiProfileDelete(
            server_, makeV1ProfileRuntime(), [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });
    registerMaintenanceWriteRoute("/api/v1/pull", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        if (!checkRateLimit())
            return;
        WifiSplitBootApiResponse::sendUnavailable(server_, WifiSplitBootApiResponse::Operation::V1_PUSH_PULL);
    });
    registerMaintenanceWriteRoute("/api/v1/push", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        if (!checkRateLimit())
            return;
        WifiSplitBootApiResponse::sendUnavailable(server_, WifiSplitBootApiResponse::Operation::V1_PUSH_PULL);
    });
    server_.on("/api/v1/current", HTTP_GET,
               [this]() { WifiV1ProfileApiService::handleApiCurrentSettings(server_, makeV1ProfileRuntime()); });
    server_.on("/api/v1/devices", HTTP_GET,
               [this]() { WifiV1DevicesApiService::handleApiDevicesList(server_, makeV1DevicesRuntime()); });
    registerMaintenanceWriteRoute("/api/v1/devices/name", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiV1DevicesApiService::handleApiDeviceNameSave(
            server_, makeV1DevicesRuntime(), [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });
    registerMaintenanceWriteRoute("/api/v1/devices/profile", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiV1DevicesApiService::handleApiDeviceProfileSave(
            server_, makeV1DevicesRuntime(), [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });
    registerMaintenanceWriteRoute("/api/v1/devices/delete", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiV1DevicesApiService::handleApiDeviceDelete(
            server_, makeV1DevicesRuntime(), [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });

    // Auto-Push routes
    server_.on("/api/autopush/slots", HTTP_GET,
               [this]() { WifiAutoPushApiService::handleApiSlots(server_, makeAutoPushRuntime()); });
    registerMaintenanceWriteRoute("/api/autopush/slot", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiAutoPushApiService::handleApiSlotSave(
            server_, makeAutoPushRuntime(), [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });
    registerMaintenanceWriteRoute("/api/autopush/activate", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiAutoPushApiService::handleApiActivate(
            server_, makeAutoPushRuntime(), [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });
    registerMaintenanceWriteRoute("/api/autopush/push", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        if (!checkRateLimit())
            return;
        WifiSplitBootApiResponse::sendUnavailable(server_, WifiSplitBootApiResponse::Operation::AUTO_PUSH_NOW);
    });
    server_.on("/api/autopush/status", HTTP_GET,
               [this]() { WifiAutoPushApiService::handleApiStatus(server_, makeAutoPushRuntime()); });

    // Display settings routes
    server_.on("/api/display/settings", HTTP_GET,
               [this]() { WifiDisplayColorsApiService::handleApiGet(server_, makeDisplayColorsRuntime()); });
    registerMaintenanceWriteRoute("/api/display/settings", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiDisplayColorsApiService::handleApiSave(
            server_, makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    registerMaintenanceWriteRoute("/api/display/settings/reset", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiDisplayColorsApiService::handleApiReset(
            server_, makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    registerMaintenanceWriteRoute("/api/display/preview", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiDisplayColorsApiService::handleApiPreview(
            server_, makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    registerMaintenanceWriteRoute("/api/display/preview/clear", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiDisplayColorsApiService::handleApiClear(
            server_, makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });

    // Audio settings routes
    server_.on("/api/audio/settings", HTTP_GET,
               [this]() { WifiAudioApiService::handleApiGet(server_, makeAudioRuntime()); });
    registerMaintenanceWriteRoute("/api/audio/settings", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiAudioApiService::handleApiSave(server_, makeAudioRuntime());
    });

    // Quiet-driving settings routes
    server_.on("/api/quiet/settings", HTTP_GET,
               [this]() { WifiQuietApiService::handleApiGet(server_, makeAudioRuntime()); });
    registerMaintenanceWriteRoute("/api/quiet/settings", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiQuietApiService::handleApiSave(server_, makeAudioRuntime());
    });

    // Settings backup/restore API routes
    server_.on("/api/settings/backup", HTTP_GET, [this]() {
        BackupApiService::handleApiBackup(
            server_, cachedBackupSnapshot_, makeBackupRuntime(),
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this,
            [](void* /*ctx*/) { return static_cast<uint32_t>(millis()); }, nullptr);
    });
    registerMaintenanceWriteRoute("/api/settings/backup-now", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        BackupApiService::handleApiBackupNow(
            server_, makeBackupRuntime(), [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this, [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    registerMaintenanceWriteRoute("/api/settings/restore", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        BackupApiService::handleApiRestore(
            server_, makeBackupRuntime(), [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this, [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });

    registerMaintenanceWriteRoute("/api/system/reboot-normal", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiSystemApiService::RebootRuntime runtime;
        runtime.maintenanceBootActive = maintenanceBootMode_;
        runtime.prepareCleanRestart = [](void* ctx) {
            auto& self = *static_cast<WiFiManager*>(ctx);
            return self.productEvents_ && self.health_ &&
                   completeLoggingForControlledRestart(*self.productEvents_, *self.health_);
        };
        runtime.persistSettings = [](void* ctx) {
            if (!static_cast<WiFiManager*>(ctx)->settings_.save()) {
                return false;
            }
            ::markCleanShutdown();
            return true;
        };
        runtime.delayBeforeRestart = [](uint32_t delayMs, void*) { delay(delayMs); };
        runtime.restart = [](void*) { ESP.restart(); };
        runtime.markUiActivity = [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); };
        runtime.ctx = this;
        WifiSystemApiService::handleApiRebootNormal(server_, runtime);
    });

    // WiFi client (STA) API routes - connect to external network
    server_.on("/api/wifi/status", HTTP_GET, [this]() {
        WifiClientApiService::handleApiStatus(
            server_, makeWifiClientRuntime(), [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); },
            this);
    });
    server_.on("/api/wifi/scan", HTTP_GET, [this]() {
        WifiClientApiService::handleApiScanStatus(
            server_, makeWifiClientRuntime(), [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); },
            this);
    });
    registerMaintenanceWriteRoute("/api/wifi/scan", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiClientApiService::handleApiScan(
            server_, makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    registerMaintenanceWriteRoute("/api/wifi/disconnect", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiClientApiService::handleApiDisconnect(
            server_, makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    registerMaintenanceWriteRoute("/api/wifi/forget", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiClientApiService::handleApiForget(
            server_, makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    registerMaintenanceWriteRoute("/api/wifi/enable", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiClientApiService::handleApiEnable(
            server_, makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/wifi/networks", HTTP_GET, [this]() {
        WifiClientApiService::handleApiNetworks(
            server_, makeWifiClientRuntime(), [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); },
            this);
    });
    registerMaintenanceWriteRoute("/api/wifi/networks", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiClientApiService::handleApiNetworksSave(
            server_, makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    registerMaintenanceWriteRoute("/api/wifi/networks/priorities", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiClientApiService::handleApiNetworksPriorities(
            server_, makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    registerMaintenanceWriteRoute("/api/wifi/networks/delete", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiClientApiService::handleApiNetworksDelete(
            server_, makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    registerMaintenanceWriteRoute("/api/wifi/networks/test", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        WifiClientApiService::handleApiNetworksTest(
            server_, makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    // OBD API routes
    server_.on("/api/obd/status", HTTP_GET, [this]() {
        markUiActivity();
        WifiSplitBootApiResponse::sendUnavailable(server_, WifiSplitBootApiResponse::Operation::OBD_RUNTIME);
    });
    server_.on("/api/obd/devices", HTTP_GET, [this]() {
        ObdApiService::handleApiDevicesList(server_, obdRuntime_, settings_, makeObdRuntime());
    });
    server_.on("/api/obd/config", HTTP_GET,
               [this]() { ObdApiService::handleApiConfigGet(server_, settings_, makeObdRuntime()); });
    registerMaintenanceWriteRoute("/api/obd/devices/name", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        ObdApiService::handleApiDeviceNameSave(server_, settings_, makeObdRuntime());
    });
    registerMaintenanceWriteRoute("/api/obd/scan", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        markUiActivity();
        if (!checkRateLimit())
            return;
        WifiSplitBootApiResponse::sendUnavailable(server_, WifiSplitBootApiResponse::Operation::OBD_RUNTIME);
    });
    registerMaintenanceWriteRoute("/api/obd/forget", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        ObdApiService::handleApiForget(server_, obdRuntime_, settings_, makeObdRuntime());
    });
    registerMaintenanceWriteRoute("/api/obd/config", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        ObdApiService::handleApiConfig(server_, obdRuntime_, settings_, makeObdRuntime());
    });

    // ALP API routes — runtime status snapshot for diagnostics/UI
    server_.on("/api/alp/status", HTTP_GET, [this]() {
        markUiActivity();
        WifiSplitBootApiResponse::sendUnavailable(server_, WifiSplitBootApiResponse::Operation::ALP_STATUS);
    });

    // GPS API routes — config + status
    server_.on("/api/gps/config", HTTP_GET, [this]() {
        GpsApiService::Runtime r;
        r.ctx = this;
        r.markUiActivity = [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); };
        GpsApiService::handleApiConfigGet(server_, settings_, r);
    });
    registerMaintenanceWriteRoute("/api/gps/config", [this]() {
        if (!requireMaintenanceWriteRequestShape())
            return;
        GpsApiService::Runtime r;
        r.ctx = this;
        r.markUiActivity = [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); };
        r.maintenanceBootActive = maintenanceBootMode_;
        GpsApiService::handleApiConfigSave(server_, settings_, gpsRuntime_, r);
    });
    server_.on("/api/gps/status", HTTP_GET, [this]() {
        markUiActivity();
        WifiSplitBootApiResponse::sendUnavailable(server_, WifiSplitBootApiResponse::Operation::GPS_STATUS);
    });

    // onNotFound is registered earlier for the remaining LittleFS assets.

    webRoutesInitialized_ = true;
    return true;
}
