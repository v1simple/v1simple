#include "wifi_status_api_service.h"

#include <esp_heap_caps.h>

#include <ArduinoJson.h>

#include "json_stream_response.h"
#include "wifi_json_document.h"

namespace WifiStatusApiService {

namespace {

constexpr size_t STATUS_CACHE_GROWTH_QUANTUM = 256u;

template <typename T> T callOr(T (*fn)(void* ctx), void* ctx, const T& fallback) {
    return fn ? fn(ctx) : fallback;
}

void buildStatusDoc(const StatusRuntime& runtime, JsonDocument& doc) {
    const bool setupModeActive = callOr<bool>(runtime.setupModeActive, runtime.setupModeActiveCtx, false);
    const bool staConnected = callOr<bool>(runtime.staConnected, runtime.staConnectedCtx, false);

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["setup_mode"] = setupModeActive;
    wifi["ap_active"] = setupModeActive;
    wifi["sta_connected"] = staConnected;
    wifi["sta_ip"] = staConnected ? callOr<String>(runtime.staIp, runtime.staIpCtx, String("")) : "";
    wifi["ap_ip"] = callOr<String>(runtime.apIp, runtime.apIpCtx, String(""));
    wifi["ssid"] = staConnected ? callOr<String>(runtime.connectedSsid, runtime.connectedSsidCtx, String(""))
                                : callOr<String>(runtime.apSsid, runtime.apSsidCtx, String(""));
    wifi["rssi"] = staConnected ? callOr<int32_t>(runtime.rssi, runtime.rssiCtx, 0) : 0;
    wifi["sta_enabled"] = callOr<bool>(runtime.staEnabled, runtime.staEnabledCtx, false);
    wifi["sta_ssid"] = callOr<String>(runtime.staSavedSsid, runtime.staSavedSsidCtx, String(""));

    JsonObject device = doc["device"].to<JsonObject>();
    device["uptime"] = callOr<unsigned long>(runtime.uptimeSeconds, runtime.uptimeSecondsCtx, 0UL);
    device["heap_free"] = callOr<uint32_t>(runtime.heapFree, runtime.heapFreeCtx, 0U);
    device["hostname"] = callOr<String>(runtime.hostname, runtime.hostnameCtx, String("v1simple"));
    device["firmware_version"] = callOr<String>(runtime.firmwareVersion, runtime.firmwareVersionCtx, String(""));

    JsonObject battery = doc["battery"].to<JsonObject>();
    battery["voltage_mv"] = callOr<uint16_t>(runtime.batteryVoltageMv, runtime.batteryVoltageMvCtx, 0);
    battery["percentage"] = callOr<uint8_t>(runtime.batteryPercentage, runtime.batteryPercentageCtx, 0);
    battery["on_battery"] = callOr<bool>(runtime.batteryOnBattery, runtime.batteryOnBatteryCtx, false);
    battery["has_battery"] = callOr<bool>(runtime.batteryHasBattery, runtime.batteryHasBatteryCtx, false);

    doc["v1_connected"] = callOr<bool>(runtime.v1Connected, runtime.v1ConnectedCtx, false);

    if (runtime.mergeStatus) {
        runtime.mergeStatus(doc.as<JsonObject>(), runtime.mergeStatusCtx);
    }
    if (runtime.mergeStatus2) {
        runtime.mergeStatus2(doc.as<JsonObject>(), runtime.mergeStatus2Ctx);
    }
    if (runtime.mergeAlert) {
        JsonObject alert = doc["alert"].to<JsonObject>();
        runtime.mergeAlert(alert, runtime.mergeAlertCtx);
    }
}

size_t roundUpStatusCacheCapacity(size_t required) {
    return ((required + STATUS_CACHE_GROWTH_QUANTUM - 1u) / STATUS_CACHE_GROWTH_QUANTUM) * STATUS_CACHE_GROWTH_QUANTUM;
}

bool ensureStatusCacheCapacity(StatusJsonCache& cachedStatusJson, size_t required) {
    if (cachedStatusJson.data != nullptr && cachedStatusJson.capacity >= required) {
        return true;
    }

    const size_t newCapacity = roundUpStatusCacheCapacity(required);
    const size_t previousCapacity = cachedStatusJson.capacity;
    char* newData = static_cast<char*>(heap_caps_malloc(newCapacity, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    bool inPsram = true;

    if (newData == nullptr) {
        Serial.printf("[WiFiStatus] Cache PSRAM alloc failed; falling back to internal (%lu bytes)\n",
                      static_cast<unsigned long>(newCapacity));
        newData = static_cast<char*>(heap_caps_malloc(newCapacity, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        inPsram = false;
    }

    if (newData == nullptr) {
        Serial.printf("[WiFiStatus] Cache alloc failed (%lu bytes); sending uncached response\n",
                      static_cast<unsigned long>(newCapacity));
        return false;
    }

    if (cachedStatusJson.data != nullptr) {
        heap_caps_free(cachedStatusJson.data);
    }

    cachedStatusJson.data = newData;
    cachedStatusJson.capacity = newCapacity;
    cachedStatusJson.length = 0;
    cachedStatusJson.inPsram = inPsram;

    Serial.printf("[WiFiStatus] Cache grow %lu -> %lu bytes (%s)\n", static_cast<unsigned long>(previousCapacity),
                  static_cast<unsigned long>(newCapacity), inPsram ? "psram" : "internal");
    return true;
}

void sendStatus(WebServer& server, const StatusRuntime& runtime, StatusJsonCache& cachedStatusJson,
                unsigned long& lastStatusJsonTime, unsigned long cacheTtlMs, unsigned long (*millisFn)(void* ctx),
                void* millisCtx) {
    const unsigned long now = millisFn ? millisFn(millisCtx) : millis();
    const bool cacheValid =
        cachedStatusJson.data != nullptr && cachedStatusJson.length > 0 && (now - lastStatusJsonTime) < cacheTtlMs;

    if (!cacheValid) {
        WifiJson::Document doc;
        buildStatusDoc(runtime, doc);

        const size_t required = measureJson(doc) + 1u;
        if (!ensureStatusCacheCapacity(cachedStatusJson, required)) {
            sendJsonStream(server, doc);
            return;
        }

        cachedStatusJson.length = serializeJson(doc, cachedStatusJson.data, cachedStatusJson.capacity);
        cachedStatusJson.data[cachedStatusJson.length] = '\0';
        lastStatusJsonTime = now;
    }

    sendSerializedJson(server, cachedStatusJson.data, cachedStatusJson.length);
}

} // namespace

void invalidateStatusJsonCache(StatusJsonCache& cachedStatusJson, unsigned long& lastStatusJsonTime) {
    cachedStatusJson.length = 0;
    lastStatusJsonTime = 0;
}

void releaseStatusJsonCache(StatusJsonCache& cachedStatusJson, unsigned long& lastStatusJsonTime) {
    if (cachedStatusJson.data != nullptr) {
        heap_caps_free(cachedStatusJson.data);
    }
    cachedStatusJson.data = nullptr;
    cachedStatusJson.capacity = 0;
    cachedStatusJson.length = 0;
    cachedStatusJson.inPsram = false;
    lastStatusJsonTime = 0;
}

void handleApiStatus(WebServer& server, const StatusRuntime& runtime, StatusJsonCache& cachedStatusJson,
                     unsigned long& lastStatusJsonTime, unsigned long cacheTtlMs, unsigned long (*millisFn)(void* ctx),
                     void* millisCtx, bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) {
        return;
    }

    sendStatus(server, runtime, cachedStatusJson, lastStatusJsonTime, cacheTtlMs, millisFn, millisCtx);
}

} // namespace WifiStatusApiService
