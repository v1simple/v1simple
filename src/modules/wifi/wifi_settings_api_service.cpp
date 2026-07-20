#include "wifi_settings_api_service.h"

#include <algorithm>

#include "../../settings_sanitize.h"
#include "wifi_api_response.h"
#include "wifi_json_document.h"

namespace WifiSettingsApiService {

namespace {

bool argIsTrue(const String& value) {
    return value == "true" || value == "1";
}

void sendSettingsUnavailable(WebServer& server) {
    server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
}

} // namespace

void handleApiDeviceSettingsGet(WebServer& server, const Runtime& runtime) {
    if (!runtime.getSettings) {
        sendSettingsUnavailable(server);
        return;
    }

    const V1Settings& settings = runtime.getSettings(runtime.ctx);

    WifiJson::Document doc;
    doc["ap_ssid"] = settings.apSSID;
    doc["ap_password"] = "********"; // Don't send actual password
    doc["isDefaultPassword"] = (settings.apPassword == "setupv1simple");
    doc["proxy_ble"] = settings.proxyBLE;
    doc["proxy_name"] = settings.proxyName;
    doc["autoPowerOffMinutes"] = settings.autoPowerOffMinutes;
    doc["apTimeoutMinutes"] = settings.apTimeoutMinutes;
    doc["alpEnabled"] = settings.alpEnabled;
    doc["alpSdLogEnabled"] = settings.alpSdLogEnabled;
    doc["alpAlertPersistSec"] = settings.alpAlertPersistSec;
    doc["alpDisableV1LaserOnPush"] = settings.alpDisableV1LaserOnPush;
    doc["powerOffSdLog"] = settings.powerOffSdLog;

    // NVS persistence diagnostics — helps verify settings actually survive reboot
    if (runtime.getNvsDiagnostic) {
        SettingsManager::NvsDiagnostic diag = runtime.getNvsDiagnostic(runtime.ctx);
        JsonObject nvs = doc["nvsDiag"].to<JsonObject>();
        nvs["ns"] = diag.activeNamespace;
        nvs["valid"] = diag.nvsValidMarker;
        nvs["ver"] = diag.settingsVersion;
        nvs["bright"] = diag.nvsBrightness;
        nvs["proxy"] = diag.nvsProxyBle;
        nvs["autoPush"] = diag.nvsAutoPush;
        nvs["healthy"] = diag.healthy;
    }

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiDeviceSettingsSave(WebServer& server, const Runtime& runtime) {
    if (runtime.checkRateLimit && !runtime.checkRateLimit(runtime.ctx))
        return;

    if (!runtime.getSettings || !runtime.applySettingsUpdate) {
        sendSettingsUnavailable(server);
        return;
    }

    const V1Settings& currentSettings = runtime.getSettings(runtime.ctx);
    DeviceSettingsUpdate update;

    if (server.hasArg("ap_ssid")) {
        String apSsid = clampStringLength(server.arg("ap_ssid"), MAX_WIFI_SSID_LEN);
        String apPass = server.arg("ap_password");
        if (apPass.length() > MAX_AP_PASSWORD_LEN && apPass != "********") {
            apPass = apPass.substring(0, MAX_AP_PASSWORD_LEN);
        }

        // If password is placeholder, keep existing password
        if (apPass == "********") {
            apPass = currentSettings.apPassword;
        }

        if (apSsid.length() == 0 || apPass.length() < 8) {
            server.send(400, "application/json",
                        "{\"error\":\"AP SSID required and password must be at least 8 characters\"}");
            return;
        }

        update.hasApCredentials = true;
        update.apSSID = apSsid;
        update.apPassword = apPass;
    }

    if (server.hasArg("proxy_ble")) {
        update.hasProxyBLE = true;
        update.proxyBLE = argIsTrue(server.arg("proxy_ble"));
    }
    if (server.hasArg("proxy_name")) {
        update.hasProxyName = true;
        update.proxyName = server.arg("proxy_name");
    }
    if (server.hasArg("autoPowerOffMinutes")) {
        int minutes = server.arg("autoPowerOffMinutes").toInt();
        minutes = std::max(0, std::min(minutes, 60));
        update.hasAutoPowerOffMinutes = true;
        update.autoPowerOffMinutes = static_cast<uint8_t>(minutes);
    }
    if (server.hasArg("apTimeoutMinutes")) {
        int minutes = server.arg("apTimeoutMinutes").toInt();
        if (minutes != 0) {
            minutes = std::max(5, std::min(minutes, 60));
        }
        update.hasApTimeoutMinutes = true;
        update.apTimeoutMinutes = static_cast<uint8_t>(minutes);
    }
    if (server.hasArg("alpEnabled")) {
        update.hasAlpEnabled = true;
        update.alpEnabled = argIsTrue(server.arg("alpEnabled"));
    }
    if (server.hasArg("alpSdLogEnabled")) {
        update.hasAlpSdLogEnabled = true;
        update.alpSdLogEnabled = argIsTrue(server.arg("alpSdLogEnabled"));
    }
    if (server.hasArg("alpAlertPersistSec")) {
        int sec = server.arg("alpAlertPersistSec").toInt();
        sec = std::max(0, std::min(sec, 5));
        update.hasAlpAlertPersistSec = true;
        update.alpAlertPersistSec = static_cast<uint8_t>(sec);
    }
    if (server.hasArg("alpDisableV1LaserOnPush")) {
        update.hasAlpDisableV1LaserOnPush = true;
        update.alpDisableV1LaserOnPush = argIsTrue(server.arg("alpDisableV1LaserOnPush"));
    }
    if (server.hasArg("powerOffSdLog")) {
        update.hasPowerOffSdLog = true;
        update.powerOffSdLog = argIsTrue(server.arg("powerOffSdLog"));
    }
    runtime.applySettingsUpdate(update, runtime.ctx);

    server.send(200, "application/json", "{\"success\":true}");
}
} // namespace WifiSettingsApiService
