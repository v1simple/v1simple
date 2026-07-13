#include "wifi_orchestrator_module.h"

#include "settings_sanitize.h"

WifiOrchestrator::WifiOrchestrator(WiFiManager& wifiManager,
                                   V1BLEClient& bleClient,
                                   PacketParser& parser,
                                   StorageManager& storageManager,
                                   AutoPushModule& autoPushModule)
    : wifiManager(wifiManager),
      bleClient(bleClient),
      parser(parser),
      storageManager(storageManager),
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
        }, this);

    // Current alert state
    wifiManager.setAlertCallback(
        [](JsonObject obj, void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            if (self->parser.hasAlerts()) {
                AlertData alert = self->parser.getPriorityAlert();
                obj["active"] = true;
                const char* bandStr = "None";
                if (alert.band == BAND_KA) bandStr = "Ka";
                else if (alert.band == BAND_K) bandStr = "K";
                else if (alert.band == BAND_X) bandStr = "X";
                else if (alert.band == BAND_LASER) bandStr = "LASER";
                obj["band"] = bandStr;
                obj["strength"] = alert.frontStrength;
                obj["frequency"] = alert.frequency;
                obj["direction"] = alert.direction;
            } else {
                obj["active"] = false;
            }
        }, this);

    // Filesystem for web APIs
    wifiManager.setFilesystemCallback(
        [](void* ctx) -> fs::FS* {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            return self->storageManager.isReady() ? self->storageManager.getFilesystem() : nullptr;
        }, this);

    // Auto-push executor status
    wifiManager.setPushStatusCallback(
        [](void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            return self->autoPushModule.getStatusJson();
        }, this);

    wifiManager.setPushNowCallback(
        [](const WifiAutoPushApiService::PushNowRequest& request, void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            AutoPushModule::PushNowRequest pushRequest;
            pushRequest.slotIndex = request.slot;
            pushRequest.activateSlot = true;
            pushRequest.hasProfileOverride = request.hasProfileOverride;
            pushRequest.hasModeOverride = request.hasModeOverride;

            if (request.hasProfileOverride) {
                pushRequest.profileName = sanitizeProfileNameValue(request.profileName);
            }
            if (request.hasModeOverride) {
                pushRequest.mode = normalizeV1ModeValue(request.mode);
            }

            switch (self->autoPushModule.queuePushNow(pushRequest)) {
                case AutoPushModule::QueueResult::QUEUED:
                    return WifiAutoPushApiService::PushNowQueueResult::QUEUED;
                case AutoPushModule::QueueResult::V1_NOT_CONNECTED:
                    return WifiAutoPushApiService::PushNowQueueResult::V1_NOT_CONNECTED;
                case AutoPushModule::QueueResult::ALREADY_IN_PROGRESS:
                    return WifiAutoPushApiService::PushNowQueueResult::ALREADY_IN_PROGRESS;
                case AutoPushModule::QueueResult::NO_PROFILE_CONFIGURED:
                    return WifiAutoPushApiService::PushNowQueueResult::NO_PROFILE_CONFIGURED;
                case AutoPushModule::QueueResult::PROFILE_LOAD_FAILED:
                default:
                    return WifiAutoPushApiService::PushNowQueueResult::PROFILE_LOAD_FAILED;
            }
        }, this);

    // V1 connection state (used to defer WiFi client operations until V1 is connected)
    wifiManager.setV1ConnectedCallback(
        [](void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            return self->bleClient.isConnected();
        }, this);
}
