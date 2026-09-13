#include "wifi_settings_api_service.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <cstdlib>

#include "../../settings_sanitize.h"
#include "exact_urlencoded_form.h"
#include "wifi_api_response.h"
#include "wifi_json_document.h"

namespace WifiSettingsApiService {

namespace {

bool argIsTrue(const String& value) {
    return value == "true" || value == "1";
}

bool exactBool(const ExactUrlEncodedForm& form, const char* key, bool& value) {
    String raw;
    if (!form.read(key, raw)) return false;
    if (raw == "true" || raw == "1") value = true;
    else if (raw == "false" || raw == "0") value = false;
    else return false;
    return true;
}

bool exactInt(const ExactUrlEncodedForm& form, const char* key, int& value) {
    String raw;
    if (!form.read(key, raw)) return false;
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(raw.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX) return false;
    value = static_cast<int>(parsed);
    return true;
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
    doc["alpAlertPersistSec"] = settings.alpAlertPersistSec;
    doc["alpDisableV1LaserOnPush"] = settings.alpDisableV1LaserOnPush;

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
    const SettingsPersistResult result = runtime.applySettingsUpdate(update, runtime.ctx);
    if (!result.success) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"settings_persist_failed\"}");
        return;
    }

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiDeviceSettingsSaveBody(WebServer& server, const Runtime& runtime,
                                     const uint8_t* body, size_t bodySize,
                                     const char* multipartBoundary,
                                     size_t multipartBoundarySize) {
    if (runtime.checkRateLimit && !runtime.checkRateLimit(runtime.ctx)) return;
    if (!runtime.getSettings || !runtime.applySettingsUpdate) {
        sendSettingsUnavailable(server);
        return;
    }
    const ExactUrlEncodedForm form = multipartBoundarySize == 0
        ? ExactUrlEncodedForm(body, bodySize)
        : ExactUrlEncodedForm(body, bodySize, multipartBoundary, multipartBoundarySize);
    if (!form.valid()) {
        server.send(400, "application/json", "{\"error\":\"Invalid form body\"}");
        return;
    }
    static constexpr const char* kAllowedFields[] = {
        "ap_ssid", "ap_password", "proxy_ble", "proxy_name", "autoPowerOffMinutes",
        "apTimeoutMinutes", "alpEnabled", "alpAlertPersistSec", "alpDisableV1LaserOnPush",
    };
    if (!form.hasOnly(kAllowedFields, sizeof(kAllowedFields) / sizeof(kAllowedFields[0])) ||
        (form.has("ap_password") != form.has("ap_ssid"))) {
        server.send(400, "application/json", "{\"error\":\"Unknown or incomplete settings field\"}");
        return;
    }

    const V1Settings& currentSettings = runtime.getSettings(runtime.ctx);
    DeviceSettingsUpdate update;
    if (form.has("ap_ssid")) {
        String apSsid;
        String apPass;
        if (!form.read("ap_ssid", apSsid) || !form.has("ap_password") ||
            !form.read("ap_password", apPass, true)) {
            server.send(503, "application/json", "{\"error\":\"Settings parameter unavailable\"}");
            return;
        }
        if (apSsid.length() > MAX_WIFI_SSID_LEN ||
            (apPass.length() > MAX_AP_PASSWORD_LEN && apPass != "********")) {
            server.send(400, "application/json", "{\"error\":\"Settings text exceeds byte limit\"}");
            return;
        }
        if (apPass == "********") {
            apPass = currentSettings.apPassword;
            if (apPass.length() != currentSettings.apPassword.length() ||
                (apPass.length() != 0 && std::memcmp(apPass.c_str(), currentSettings.apPassword.c_str(),
                                                     apPass.length()) != 0)) {
                server.send(503, "application/json", "{\"error\":\"Settings memory unavailable\"}");
                return;
            }
        }
        if (apSsid.length() == 0 || apPass.length() < 8) {
            server.send(400, "application/json",
                        "{\"error\":\"AP SSID required and password must be at least 8 characters\"}");
            return;
        }
        update.hasApCredentials = true;
        update.apSSID = std::move(apSsid);
        update.apPassword = std::move(apPass);
    }

    if (form.has("proxy_ble")) {
        update.hasProxyBLE = true;
        if (!exactBool(form, "proxy_ble", update.proxyBLE)) {
            server.send(400, "application/json", "{\"error\":\"Invalid proxy setting\"}");
            return;
        }
    }
    if (form.has("proxy_name")) {
        update.hasProxyName = true;
        if (!form.read("proxy_name", update.proxyName, true)) {
            server.send(503, "application/json", "{\"error\":\"Settings parameter unavailable\"}");
            return;
        }
        if (update.proxyName.length() > MAX_PROXY_NAME_LEN) {
            server.send(400, "application/json", "{\"error\":\"Proxy name exceeds byte limit\"}");
            return;
        }
    }
    int parsed = 0;
    if (form.has("autoPowerOffMinutes")) {
        if (!exactInt(form, "autoPowerOffMinutes", parsed)) {
            server.send(400, "application/json", "{\"error\":\"Invalid power-off setting\"}");
            return;
        }
        update.hasAutoPowerOffMinutes = true;
        if (parsed < 0 || parsed > 60) {
            server.send(400, "application/json", "{\"error\":\"Invalid power-off setting\"}");
            return;
        }
        update.autoPowerOffMinutes = static_cast<uint8_t>(parsed);
    }
    if (form.has("apTimeoutMinutes")) {
        if (!exactInt(form, "apTimeoutMinutes", parsed)) {
            server.send(400, "application/json", "{\"error\":\"Invalid AP timeout\"}");
            return;
        }
        if (parsed != 0 && (parsed < 5 || parsed > 60)) {
            server.send(400, "application/json", "{\"error\":\"Invalid AP timeout\"}");
            return;
        }
        update.hasApTimeoutMinutes = true;
        update.apTimeoutMinutes = static_cast<uint8_t>(parsed);
    }
    if (form.has("alpEnabled")) {
        update.hasAlpEnabled = true;
        if (!exactBool(form, "alpEnabled", update.alpEnabled)) {
            server.send(400, "application/json", "{\"error\":\"Invalid ALP setting\"}");
            return;
        }
    }
    if (form.has("alpAlertPersistSec")) {
        if (!exactInt(form, "alpAlertPersistSec", parsed)) {
            server.send(400, "application/json", "{\"error\":\"Invalid ALP persistence\"}");
            return;
        }
        update.hasAlpAlertPersistSec = true;
        if (parsed < 0 || parsed > 5) {
            server.send(400, "application/json", "{\"error\":\"Invalid ALP persistence\"}");
            return;
        }
        update.alpAlertPersistSec = static_cast<uint8_t>(parsed);
    }
    if (form.has("alpDisableV1LaserOnPush")) {
        update.hasAlpDisableV1LaserOnPush = true;
        if (!exactBool(form, "alpDisableV1LaserOnPush", update.alpDisableV1LaserOnPush)) {
            server.send(400, "application/json", "{\"error\":\"Invalid ALP laser setting\"}");
            return;
        }
    }
    const SettingsPersistResult result = runtime.applySettingsUpdate(update, runtime.ctx);
    if (!result.success) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"settings_persist_failed\"}");
        return;
    }
    server.send(200, "application/json", "{\"success\":true}");
}
} // namespace WifiSettingsApiService
