#include "wifi_orchestrator_module.h"

WifiOrchestrator::WifiOrchestrator(WiFiManager& wifiManager, V1BLEClient& bleClient, PacketParser& parser,
                                   StorageManager& storageManager, AutoPushModule& autoPushModule)
    : wifiManager(wifiManager), bleClient(bleClient), parser(parser), storageManager(storageManager),
      autoPushModule(autoPushModule) {}

void WifiOrchestrator::ensureCallbacksConfigured() {
    if (!callbacksConfigured_) {
        configureCallbacks();
        callbacksConfigured_ = true;
    }
}

void WifiOrchestrator::configureCallbacks() {
    // V1 connection status
    wifiManager.setStatusCallback(
        [](JsonObject obj, void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            obj["v1_connected"] = self->bleClient.isConnected();
        },
        this);

    // Current alert state
    wifiManager.setAlertCallback(
        [](JsonObject obj, void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            if (self->parser.hasAlerts()) {
                AlertData alert = self->parser.getPriorityAlert();
                obj["active"] = true;
                const char* bandStr = "None";
                if (alert.band == BAND_KA)
                    bandStr = "Ka";
                else if (alert.band == BAND_K)
                    bandStr = "K";
                else if (alert.band == BAND_X)
                    bandStr = "X";
                else if (alert.band == BAND_LASER)
                    bandStr = "LASER";
                obj["band"] = bandStr;
                obj["strength"] = alert.frontStrength;
                obj["frequency"] = alert.frequency;
                obj["direction"] = alert.direction;
            } else {
                obj["active"] = false;
            }
        },
        this);

    // Filesystem for web APIs
    wifiManager.setFilesystemCallback(
        [](void* ctx) -> fs::FS* {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            return self->storageManager.isReady() ? self->storageManager.getFilesystem() : nullptr;
        },
        this);

    // Auto-push executor status
    wifiManager.setPushStatusCallback(
        [](void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            return self->autoPushModule.getStatusJson();
        },
        this);

    // Defer WiFi client operations until the V1 connection is ready.
    wifiManager.setV1ConnectedCallback(
        [](void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            return self->bleClient.isConnected();
        },
        this);
}
