#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <cstdint>
#include <vector>

namespace WifiClientApiService {

struct StatusPayload {
    bool enabled = false;
    String savedSsid;
    const char* state = "unknown";
    bool scanRunning = false;
    bool includeConnectedFields = false;
    String connectedSsid;
    int connectedSlotIndex = -1;
    String ip;
    int32_t rssi = 0;
};

struct ScannedNetworkPayload {
    String ssid;
    int32_t rssi = 0;
    bool secure = true;
};

struct ConnectedNetworkPayload {
    String ssid;
    int connectedSlotIndex = -1;
    String ip;
    int32_t rssi = 0;
};

struct SavedNetworkSlotPayload {
    size_t index = 0;
    String ssid;
    String label;
    uint8_t priority = 0;
    uint32_t lastConnectedAtSec = 0;
    bool configured = false;
    bool hasPassword = false;
};

struct SavedNetworkUpsertPayload {
    bool hasIndex = false;
    size_t index = 0;
    String ssid;
    bool hasPassword = false;
    String password;
    bool hasLabel = false;
    String label;
    bool hasPriority = false;
    uint8_t priority = 0;
};

struct Runtime {
    bool (*isEnabled)(void* ctx) = nullptr;
    void* isEnabledCtx = nullptr;
    String (*getSavedSsid)(void* ctx) = nullptr;
    void* getSavedSsidCtx = nullptr;
    const char* (*getStateName)(void* ctx) = nullptr;
    void* getStateNameCtx = nullptr;
    bool (*isScanRunning)(void* ctx) = nullptr;
    void* isScanRunningCtx = nullptr;
    bool (*isConnected)(void* ctx) = nullptr;
    void* isConnectedCtx = nullptr;
    ConnectedNetworkPayload (*getConnectedNetwork)(void* ctx) = nullptr;
    void* getConnectedNetworkCtx = nullptr;

    bool (*isScanInProgress)(void* ctx) = nullptr;
    void* isScanInProgressCtx = nullptr;
    bool (*hasCompletedScanResults)(void* ctx) = nullptr;
    void* hasCompletedScanResultsCtx = nullptr;
    std::vector<ScannedNetworkPayload> (*getScannedNetworks)(void* ctx) = nullptr;
    void* getScannedNetworksCtx = nullptr;
    bool (*startScan)(void* ctx) = nullptr;
    void* startScanCtx = nullptr;

    void (*disconnectFromNetwork)(void* ctx) = nullptr;
    void* disconnectFromNetworkCtx = nullptr;
    void (*forgetClient)(void* ctx) = nullptr;
    void* forgetClientCtx = nullptr;
    bool (*enableWithSavedNetwork)(void* ctx) = nullptr;
    void* enableWithSavedNetworkCtx = nullptr;
    void (*disableClient)(void* ctx) = nullptr;
    void* disableClientCtx = nullptr;

    bool maintenanceBootActive = false;

    std::vector<SavedNetworkSlotPayload> (*getSavedNetworks)(void* ctx) = nullptr;
    void* getSavedNetworksCtx = nullptr;
    bool (*upsertSavedNetwork)(const SavedNetworkUpsertPayload& request,
                               size_t& indexOut,
                               void* ctx) = nullptr;
    void* upsertSavedNetworkCtx = nullptr;
    bool (*deleteSavedNetwork)(size_t index, void* ctx) = nullptr;
    void* deleteSavedNetworkCtx = nullptr;
    bool (*testSavedNetwork)(size_t index, void* ctx) = nullptr;
    void* testSavedNetworkCtx = nullptr;
};

void handleApiStatus(WebServer& server,
                     const Runtime& runtime,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiScan(WebServer& server,
                   const Runtime& runtime,
                   bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                   void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiScanStatus(WebServer& server,
                         const Runtime& runtime,
                         void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiDisconnect(WebServer& server,
                         const Runtime& runtime,
                         bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                         void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiForget(WebServer& server,
                     const Runtime& runtime,
                     bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiEnable(WebServer& server,
                     const Runtime& runtime,
                     bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiNetworks(WebServer& server,
                       const Runtime& runtime,
                       void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiNetworksSave(WebServer& server,
                           const Runtime& runtime,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                           void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiNetworksDelete(WebServer& server,
                             const Runtime& runtime,
                             bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                             void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiNetworksTest(WebServer& server,
                           const Runtime& runtime,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                           void (*markUiActivity)(void* ctx), void* uiActivityCtx);

}  // namespace WifiClientApiService
