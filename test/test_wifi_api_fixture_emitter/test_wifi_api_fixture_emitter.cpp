#include <unity.h>

#include <ArduinoJson.h>

#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/modules/gps/gps_api_service.h"
#include "../../src/modules/gps/gps_runtime_module.h"
#include "../../src/modules/wifi/backup_api_service.h"
#include "../../src/modules/wifi/wifi_audio_api_service.h"
#include "../../src/modules/wifi/wifi_autopush_api_service.h"
#include "../../src/modules/wifi/wifi_client_api_service.h"
#include "../../src/modules/wifi/wifi_client_enable_transaction.h"
#include "../../src/modules/wifi/wifi_client_enable_transaction.cpp"
#include "../../src/modules/wifi/wifi_display_colors_api_service.h"
#include "../../src/modules/wifi/wifi_quiet_api_service.h"
#include "../../src/modules/wifi/wifi_settings_api_service.h"

SettingsManager::SettingsManager() {
    settings_.gpsEnabled = true;
    settings_.gpsBaud = 9600;
    settings_.gpsLogUtcToPerf = true;
    settings_.gpsLogUtcToAlp = true;
}

void SettingsManager::applyDeviceSettingsUpdate(const DeviceSettingsUpdate& update, SettingsPersistMode /*mode*/) {
    if (update.hasGpsEnabled) {
        settings_.gpsEnabled = update.gpsEnabled;
    }
    if (update.hasGpsBaud) {
        settings_.gpsBaud = update.gpsBaud;
    }
    if (update.hasGpsLogUtcToPerf) {
        settings_.gpsLogUtcToPerf = update.gpsLogUtcToPerf;
    }
    if (update.hasGpsLogUtcToAlp) {
        settings_.gpsLogUtcToAlp = update.gpsLogUtcToAlp;
    }
}

void GpsRuntimeModule::setEnabled(bool enabled) {
    enabled_ = enabled;
}

void GpsRuntimeModule::setBaud(uint32_t baud) {
    baud_ = baud;
}

void GpsRuntimeModule::setEnablePinActiveHigh(bool activeHigh) {
    enablePinActiveHigh_ = activeHigh;
}

GpsRuntimeStatus GpsRuntimeModule::snapshot(uint32_t /*nowMs*/) const {
    GpsRuntimeStatus status;
    status.enabled = enabled_;
    status.moduleDetected = true;
    status.parserActive = enabled_;
    status.hasFix = true;
    status.stableHasFix = true;
    status.satellites = 7;
    status.stableSatellites = 7;
    status.hdop = 1.2F;
    status.speedMph = 35.5F;
    status.locationValid = true;
    status.fixAgeMs = 800;
    status.stableFixAgeMs = 800;
    status.sampleAgeMs = 500;
    status.lastSentenceAgeMs = 300;
    status.firstFixMs = 100;
    status.sentencesParsed = 42;
    status.parseFailures = 1;
    status.bytesRead = 4096;
    status.enableTransitions = 1;
    return status;
}

namespace BackupPayloadBuilder {

bool isRecognizedBackupType(const char* type) {
    return type != nullptr &&
           (std::strcmp(type, "v1simple_backup") == 0 || std::strcmp(type, "v1simple_sd_backup") == 0);
}

} // namespace BackupPayloadBuilder

namespace BackupApiService {

bool sendCachedBackupSnapshot(WebServer& server, BackupSnapshotCache& /*cache*/, uint32_t /*settingsRevision*/,
                              uint32_t /*profileRevision*/, BackupSnapshotBuildFn /*buildSnapshot*/, void* /*buildCtx*/,
                              uint32_t (* /*millisFn*/)(void* ctx), void* /*millisCtx*/) {
    server.send(200, "application/json", "{\"cached\":true}");
    return true;
}

} // namespace BackupApiService

#include "../../src/modules/wifi/backup_api_service.cpp"
#include "../../src/modules/gps/gps_api_service.cpp"
#include "../../src/modules/wifi/wifi_audio_api_service.cpp"
#include "../../src/modules/wifi/wifi_autopush_api_service.cpp"
#include "../../src/modules/wifi/wifi_client_api_service.cpp"
#include "../../src/modules/wifi/wifi_display_colors_api_service.cpp"
#include "../../src/modules/wifi/wifi_quiet_api_service.cpp"
#include "../../src/modules/wifi/wifi_settings_api_service.cpp"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {

constexpr const char* kFixtureMarker = "V1_WIFI_API_FIXTURE=";

struct FakeRuntime {
    bool enabled = false;
    String savedSsid;
    const char* stateName = "disabled";
    bool scanRunning = false;
    bool connected = false;
    WifiClientApiService::ConnectedNetworkPayload connectedNetwork;

    bool scanInProgress = false;
    bool hasCompletedResults = false;
    bool maintenanceBootActive = false;
    std::vector<WifiClientApiService::ScannedNetworkPayload> scannedNetworks;
    std::vector<WifiClientApiService::SavedNetworkSlotPayload> savedNetworks;
    bool startScanResult = false;
    bool enableResult = true;
};

struct RestoreState {
    V1Settings settings;
    FakeRuntime wifi;
    int applyCalls = 0;
    int syncCalls = 0;
};

struct SettingsCaptureState {
    V1Settings settings;
    int audioApplyCalls = 0;
    int displayApplyCalls = 0;
    int displayResetCalls = 0;
    int quietApplyCalls = 0;
    int audioVolumeCalls = 0;
    bool previewRunning = false;
};

struct CapturedResponse {
    const char* scenario = nullptr;
    const char* route = nullptr;
    int status = 0;
    std::string contentType;
    std::string body;
};

WifiClientApiService::Runtime makeRuntime(FakeRuntime& state) {
    WifiClientApiService::Runtime runtime;
    runtime.isEnabled = [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->enabled; };
    runtime.isEnabledCtx = &state;
    runtime.getSavedSsid = [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->savedSsid; };
    runtime.getSavedSsidCtx = &state;
    runtime.getStateName = [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->stateName; };
    runtime.getStateNameCtx = &state;
    runtime.isScanRunning = [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->scanRunning; };
    runtime.isScanRunningCtx = &state;
    runtime.isConnected = [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->connected; };
    runtime.isConnectedCtx = &state;
    runtime.getConnectedNetwork = [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->connectedNetwork; };
    runtime.getConnectedNetworkCtx = &state;
    runtime.isScanInProgress = [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->scanInProgress; };
    runtime.isScanInProgressCtx = &state;
    runtime.hasCompletedScanResults = [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->hasCompletedResults; };
    runtime.hasCompletedScanResultsCtx = &state;
    runtime.getScannedNetworks = [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->scannedNetworks; };
    runtime.getScannedNetworksCtx = &state;
    runtime.startScan = [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->startScanResult; };
    runtime.startScanCtx = &state;
    runtime.maintenanceBootActive = state.maintenanceBootActive;
    runtime.getSavedNetworks = [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->savedNetworks; };
    runtime.getSavedNetworksCtx = &state;
    runtime.enableWithSavedNetwork = [](void* ctx) {
        auto* state = static_cast<FakeRuntime*>(ctx);
        WifiClientEnableTransaction::Runtime transaction;
        transaction.ctx = state;
        transaction.attemptStart = [](void* transactionCtx) {
            return static_cast<FakeRuntime*>(transactionCtx)->enableResult;
        };
        transaction.rollbackFailedStart = [](void* transactionCtx) {
            auto* transactionState = static_cast<FakeRuntime*>(transactionCtx);
            transactionState->stateName = "disabled";
        };
        transaction.commitEnabled = [](void* transactionCtx) {
            auto* transactionState = static_cast<FakeRuntime*>(transactionCtx);
            transactionState->enabled = true;
        };
        return WifiClientEnableTransaction::execute(transaction);
    };
    runtime.enableWithSavedNetworkCtx = &state;
    runtime.disableClient = [](void*) {};
    runtime.disableClientCtx = &state;
    return runtime;
}

WifiSettingsApiService::Runtime makeSettingsRuntime(V1Settings& settings) {
    WifiSettingsApiService::Runtime runtime;
    runtime.getSettings = [](void* ctx) -> const V1Settings& { return *static_cast<V1Settings*>(ctx); };
    runtime.ctx = &settings;
    return runtime;
}

BackupApiService::BackupRuntime makeRestoreRuntime(RestoreState& state) {
    BackupApiService::BackupRuntime runtime;
    runtime.ctx = &state;
    runtime.applyBackup = [](const JsonDocument& /*doc*/, bool fullRestore, int& profilesRestored, void* ctx) {
        auto* restore = static_cast<RestoreState*>(ctx);
        TEST_ASSERT_TRUE(fullRestore);
        restore->applyCalls++;
        restore->settings.apSSID = "RestoredAP";
        restore->wifi.enabled = true;
        restore->wifi.savedSsid = "RestoredWifi";
        restore->wifi.stateName = "connected";
        restore->wifi.connected = true;
        restore->wifi.connectedNetwork.ssid = "RestoredWifi";
        restore->wifi.connectedNetwork.connectedSlotIndex = 0;
        restore->wifi.connectedNetwork.ip = "192.168.1.50";
        restore->wifi.connectedNetwork.rssi = -48;
        restore->wifi.savedNetworks.clear();
        WifiClientApiService::SavedNetworkSlotPayload slot;
        slot.index = 0;
        slot.ssid = "RestoredWifi";
        slot.label = "Restored Network";
        slot.priority = 0;
        slot.lastConnectedAtSec = 321;
        slot.configured = true;
        slot.hasPassword = true;
        restore->wifi.savedNetworks.push_back(slot);
        profilesRestored = 2;
        return true;
    };
    runtime.syncAfterRestore = [](void* ctx) { static_cast<RestoreState*>(ctx)->syncCalls++; };
    return runtime;
}

WifiAudioApiService::Runtime makeAudioRuntime(SettingsCaptureState& state) {
    WifiAudioApiService::Runtime runtime;
    runtime.getSettings = [](void* ctx) -> const V1Settings& {
        return static_cast<SettingsCaptureState*>(ctx)->settings;
    };
    runtime.applySettingsUpdate = [](const AudioSettingsUpdate& /*update*/, void* ctx) {
        static_cast<SettingsCaptureState*>(ctx)->audioApplyCalls++;
    };
    runtime.setAudioVolume = [](uint8_t /*volume*/, void* ctx) {
        static_cast<SettingsCaptureState*>(ctx)->audioVolumeCalls++;
    };
    runtime.checkRateLimit = [](void* /*ctx*/) { return true; };
    runtime.ctx = &state;
    return runtime;
}

WifiDisplayColorsApiService::Runtime makeDisplayRuntime(SettingsCaptureState& state) {
    WifiDisplayColorsApiService::Runtime runtime;
    runtime.getSettings = [](void* ctx) -> const V1Settings& {
        return static_cast<SettingsCaptureState*>(ctx)->settings;
    };
    runtime.getSettingsCtx = &state;
    runtime.applySettingsUpdate = [](const DisplaySettingsUpdate& /*update*/, void* ctx) {
        static_cast<SettingsCaptureState*>(ctx)->displayApplyCalls++;
    };
    runtime.applySettingsUpdateCtx = &state;
    runtime.resetDisplaySettings = [](void* ctx) { static_cast<SettingsCaptureState*>(ctx)->displayResetCalls++; };
    runtime.resetDisplaySettingsCtx = &state;
    runtime.requestColorPreviewHoldMs = [](uint32_t /*durationMs*/, void* ctx) {
        static_cast<SettingsCaptureState*>(ctx)->previewRunning = true;
    };
    runtime.requestColorPreviewHoldMsCtx = &state;
    runtime.isColorPreviewRunning = [](void* ctx) { return static_cast<SettingsCaptureState*>(ctx)->previewRunning; };
    runtime.isColorPreviewRunningCtx = &state;
    runtime.cancelColorPreview = [](void* ctx) { static_cast<SettingsCaptureState*>(ctx)->previewRunning = false; };
    runtime.cancelColorPreviewCtx = &state;
    return runtime;
}

WifiQuietApiService::Runtime makeQuietRuntime(SettingsCaptureState& state) {
    WifiQuietApiService::Runtime runtime;
    runtime.getSettings = [](void* ctx) -> const V1Settings& {
        return static_cast<SettingsCaptureState*>(ctx)->settings;
    };
    runtime.applySettingsUpdate = [](const QuietSettingsUpdate& /*update*/, void* ctx) {
        static_cast<SettingsCaptureState*>(ctx)->quietApplyCalls++;
    };
    runtime.checkRateLimit = [](void* /*ctx*/) { return true; };
    runtime.ctx = &state;
    return runtime;
}

void captureResponse(std::vector<CapturedResponse>& captures, const char* scenarioName, const char* routeKey,
                     const WebServer& server) {
    TEST_ASSERT_GREATER_OR_EQUAL_INT(100, server.lastStatusCode);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, server.lastContentType.length(), routeKey);

    JsonDocument body;
    const DeserializationError error = deserializeJson(body, server.lastBody.c_str());
    TEST_ASSERT_FALSE_MESSAGE(static_cast<bool>(error), server.lastBody.c_str());

    captures.push_back(CapturedResponse{scenarioName, routeKey, server.lastStatusCode, server.lastContentType.c_str(),
                                        server.lastBody.c_str()});
}

void captureStatus(std::vector<CapturedResponse>& captures, const char* scenarioName, FakeRuntime& state) {
    WebServer server(80);
    WifiClientApiService::handleApiStatus(server, makeRuntime(state), nullptr, nullptr);
    captureResponse(captures, scenarioName, "GET /api/wifi/status", server);
}

void captureSettings(std::vector<CapturedResponse>& captures, const char* scenarioName, V1Settings& settings) {
    WebServer server(80);
    WifiSettingsApiService::handleApiDeviceSettingsGet(server, makeSettingsRuntime(settings));
    captureResponse(captures, scenarioName, "GET /api/device/settings", server);
}

void captureSavedNetworks(std::vector<CapturedResponse>& captures, const char* scenarioName, FakeRuntime& state) {
    WebServer server(80);
    WifiClientApiService::handleApiNetworks(server, makeRuntime(state), nullptr, nullptr);
    captureResponse(captures, scenarioName, "GET /api/wifi/networks", server);
}

void emitFixture() {
    std::vector<CapturedResponse> captures;

    FakeRuntime defaultStatus;
    defaultStatus.enabled = true;
    defaultStatus.stateName = "disconnected";
    captureStatus(captures, "wifi_status_default", defaultStatus);

    FakeRuntime scanStart;
    scanStart.startScanResult = true;
    WebServer scanStartServer(80);
    WifiClientApiService::handleApiScan(scanStartServer, makeRuntime(scanStart), nullptr, nullptr, nullptr, nullptr);
    captureResponse(captures, "wifi_scan_success", "POST /api/wifi/scan", scanStartServer);

    FakeRuntime scanRunning;
    scanRunning.scanRunning = true;
    scanRunning.scanInProgress = true;
    WebServer scanRunningServer(80);
    WifiClientApiService::handleApiScanStatus(scanRunningServer, makeRuntime(scanRunning), nullptr, nullptr);
    captureResponse(captures, "wifi_scan_success", "GET /api/wifi/scan", scanRunningServer);

    FakeRuntime scanComplete;
    scanComplete.scanRunning = true;
    scanComplete.hasCompletedResults = true;
    WifiClientApiService::ScannedNetworkPayload network;
    network.ssid = "BenchAP";
    network.rssi = -42;
    network.secure = true;
    scanComplete.scannedNetworks.push_back(network);
    WebServer scanCompleteServer(80);
    WifiClientApiService::handleApiScanStatus(scanCompleteServer, makeRuntime(scanComplete), nullptr, nullptr);
    captureResponse(captures, "wifi_scan_success", "GET /api/wifi/scan", scanCompleteServer);

    FakeRuntime enableSuccessBefore;
    captureStatus(captures, "wifi_enable_success", enableSuccessBefore);

    FakeRuntime enableSuccess;
    enableSuccess.enableResult = true;
    WebServer enableSuccessServer(80);
    enableSuccessServer.setArg("plain", "{\"enabled\":true}");
    WifiClientApiService::handleApiEnable(enableSuccessServer, makeRuntime(enableSuccess), nullptr, nullptr, nullptr,
                                          nullptr);
    captureResponse(captures, "wifi_enable_success", "POST /api/wifi/enable", enableSuccessServer);

    FakeRuntime enableSuccessAfter;
    enableSuccessAfter.enabled = true;
    enableSuccessAfter.savedSsid = "HomeWifi";
    enableSuccessAfter.stateName = "connecting";
    captureStatus(captures, "wifi_enable_success", enableSuccessAfter);

    FakeRuntime enableFailureBefore;
    enableFailureBefore.maintenanceBootActive = true;
    enableFailureBefore.savedSsid = "GarageNet";
    WifiClientApiService::SavedNetworkSlotPayload configuredSlot;
    configuredSlot.index = 0;
    configuredSlot.ssid = "GarageNet";
    configuredSlot.configured = true;
    enableFailureBefore.savedNetworks.push_back(configuredSlot);
    captureStatus(captures, "wifi_enable_failure", enableFailureBefore);

    FakeRuntime enableFailure = enableFailureBefore;
    enableFailure.enableResult = false;
    WebServer enableFailureServer(80);
    enableFailureServer.setArg("plain", "{\"enabled\":true}");
    WifiClientApiService::handleApiEnable(enableFailureServer, makeRuntime(enableFailure), nullptr, nullptr, nullptr,
                                          nullptr);
    captureResponse(captures, "wifi_enable_failure", "POST /api/wifi/enable", enableFailureServer);

    captureStatus(captures, "wifi_enable_failure", enableFailure);

    RestoreState restore;
    restore.settings.apSSID = "BeforeRestoreAP";
    restore.settings.apPassword = "before-restore";
    restore.wifi.enabled = false;
    restore.wifi.stateName = "disabled";
    restore.wifi.maintenanceBootActive = true;
    WifiClientApiService::SavedNetworkSlotPayload oldSlot;
    oldSlot.index = 0;
    oldSlot.ssid = "OldWifi";
    oldSlot.label = "Old Network";
    oldSlot.priority = 0;
    oldSlot.configured = true;
    oldSlot.hasPassword = true;
    restore.wifi.savedNetworks.push_back(oldSlot);

    captureSettings(captures, "settings_restore_success", restore.settings);
    captureStatus(captures, "settings_restore_success", restore.wifi);
    captureSavedNetworks(captures, "settings_restore_success", restore.wifi);

    WebServer restoreServer(80);
    restoreServer.setArg("plain", "{\"_type\":\"v1simple_backup\",\"profiles\":[]}");
    BackupApiService::handleApiRestore(restoreServer, makeRestoreRuntime(restore), nullptr, nullptr, nullptr, nullptr);
    captureResponse(captures, "settings_restore_success", "POST /api/settings/restore", restoreServer);

    captureSettings(captures, "settings_restore_success", restore.settings);
    captureStatus(captures, "settings_restore_success", restore.wifi);
    captureSavedNetworks(captures, "settings_restore_success", restore.wifi);

    TEST_ASSERT_EQUAL_INT(1, restore.applyCalls);
    TEST_ASSERT_EQUAL_INT(1, restore.syncCalls);

    SettingsCaptureState settingsCapture;
    settingsCapture.settings.voiceAlertMode = VOICE_MODE_BAND_FREQ;
    settingsCapture.settings.voiceDirectionEnabled = true;
    settingsCapture.settings.announceBogeyCount = true;
    settingsCapture.settings.muteVoiceIfVolZero = false;
    settingsCapture.settings.voiceVolume = 72;
    settingsCapture.settings.announceSecondaryAlerts = true;
    settingsCapture.settings.secondaryLaser = true;
    settingsCapture.settings.secondaryKa = true;
    settingsCapture.settings.secondaryK = false;
    settingsCapture.settings.secondaryX = false;
    settingsCapture.settings.alertVolumeFadeEnabled = true;
    settingsCapture.settings.alertVolumeFadeDelaySec = 4;
    settingsCapture.settings.alertVolumeFadeVolume = 2;
    settingsCapture.settings.speedMuteEnabled = true;
    settingsCapture.settings.speedMuteThresholdMph = 31;
    settingsCapture.settings.speedMuteHysteresisMph = 6;
    settingsCapture.settings.speedMuteVolume = 3;
    settingsCapture.settings.speedMuteVoice = false;
    settingsCapture.settings.stealthEnabled = true;
    settingsCapture.settings.brightness = 123;
    settingsCapture.settings.hideBatteryIcon = false;
    settingsCapture.settings.showBatteryPercent = true;

    WebServer audioGetServer(80);
    WifiAudioApiService::handleApiGet(audioGetServer, makeAudioRuntime(settingsCapture));
    captureResponse(captures, "audio_settings_success", "GET /api/audio/settings", audioGetServer);

    WebServer audioSaveServer(80);
    audioSaveServer.setArg("voiceAlertMode", "3");
    audioSaveServer.setArg("voiceVolume", "72");
    audioSaveServer.setArg("stealthEnabled", "false");
    WifiAudioApiService::handleApiSave(audioSaveServer, makeAudioRuntime(settingsCapture));
    captureResponse(captures, "audio_settings_success", "POST /api/audio/settings", audioSaveServer);

    WebServer displayGetServer(80);
    WifiDisplayColorsApiService::handleApiGet(displayGetServer, makeDisplayRuntime(settingsCapture));
    captureResponse(captures, "display_colors_success", "GET /api/display/settings", displayGetServer);

    WebServer displaySaveServer(80);
    displaySaveServer.setArg("brightness", "123");
    displaySaveServer.setArg("skipPreview", "true");
    WifiDisplayColorsApiService::handleApiSave(displaySaveServer, makeDisplayRuntime(settingsCapture), nullptr,
                                               nullptr);
    captureResponse(captures, "display_colors_success", "POST /api/display/settings", displaySaveServer);

    WebServer displayResetServer(80);
    WifiDisplayColorsApiService::handleApiReset(displayResetServer, makeDisplayRuntime(settingsCapture), nullptr,
                                                nullptr);
    captureResponse(captures, "display_colors_success", "POST /api/display/settings/reset", displayResetServer);

    WebServer displayClearServer(80);
    WifiDisplayColorsApiService::handleApiClear(displayClearServer, makeDisplayRuntime(settingsCapture), nullptr,
                                                nullptr);
    captureResponse(captures, "display_colors_success", "POST /api/display/preview/clear", displayClearServer);

    WebServer displayPreviewServer(80);
    WifiDisplayColorsApiService::handleApiPreview(displayPreviewServer, makeDisplayRuntime(settingsCapture), nullptr,
                                                  nullptr);
    captureResponse(captures, "display_colors_success", "POST /api/display/preview", displayPreviewServer);

    settingsCapture.settings.stealthEnabled = false;
    WebServer quietGetServer(80);
    WifiQuietApiService::handleApiGet(quietGetServer, makeQuietRuntime(settingsCapture));
    captureResponse(captures, "quiet_settings_success", "GET /api/quiet/settings", quietGetServer);

    WebServer quietSaveServer(80);
    quietSaveServer.setArg("stealthEnabled", "true");
    WifiQuietApiService::handleApiSave(quietSaveServer, makeQuietRuntime(settingsCapture));
    captureResponse(captures, "quiet_settings_success", "POST /api/quiet/settings", quietSaveServer);

    SettingsManager gpsSettings;
    GpsRuntimeModule gpsRuntime;
    gpsRuntime.setEnabled(true);
    GpsApiService::Runtime gpsApiRuntime;

    WebServer gpsConfigServer(80);
    GpsApiService::handleApiConfigGet(gpsConfigServer, gpsSettings, gpsApiRuntime);
    captureResponse(captures, "gps_settings_and_status", "GET /api/gps/config", gpsConfigServer);

    WebServer gpsStatusServer(80);
    GpsApiService::handleApiStatus(gpsStatusServer, gpsRuntime, gpsApiRuntime);
    captureResponse(captures, "gps_settings_and_status", "GET /api/gps/status", gpsStatusServer);

    WebServer gpsSaveServer(80);
    gpsSaveServer.setArg("plain", "{\"gpsEnabled\":false}");
    GpsApiService::handleApiConfigSave(gpsSaveServer, gpsSettings, gpsRuntime, gpsApiRuntime);
    captureResponse(captures, "gps_settings_and_status", "POST /api/gps/config", gpsSaveServer);

    TEST_ASSERT_EQUAL_INT(1, settingsCapture.audioApplyCalls);
    TEST_ASSERT_EQUAL_INT(1, settingsCapture.audioVolumeCalls);
    TEST_ASSERT_EQUAL_INT(1, settingsCapture.displayApplyCalls);
    TEST_ASSERT_EQUAL_INT(1, settingsCapture.displayResetCalls);
    TEST_ASSERT_EQUAL_INT(1, settingsCapture.quietApplyCalls);
    TEST_ASSERT_EQUAL_UINT32(29, captures.size());

    std::ostringstream outputStream;
    outputStream << "{\"schemaVersion\":1,\"captures\":[";
    for (size_t index = 0; index < captures.size(); ++index) {
        const CapturedResponse& capture = captures[index];
        if (index > 0) {
            outputStream << ',';
        }
        outputStream << "{\"scenario\":\"" << capture.scenario << "\",\"route\":\"" << capture.route
                     << "\",\"status\":" << capture.status << ",\"contentType\":\"" << capture.contentType
                     << "\",\"body\":" << capture.body << '}';
    }
    outputStream << "]}";
    const std::string output = outputStream.str();
    const char* outputPath = std::getenv("V1_WIFI_API_FIXTURE_OUTPUT");
    if (outputPath && outputPath[0] != '\0') {
        std::ofstream outputFile(outputPath, std::ios::binary | std::ios::trunc);
        TEST_ASSERT_TRUE_MESSAGE(outputFile.is_open(), outputPath);
        outputFile << output;
        outputFile.close();
        TEST_ASSERT_TRUE_MESSAGE(outputFile.good(), outputPath);
        return;
    }

    const std::string fixtureMessage = std::string(kFixtureMarker) + output;
    TEST_MESSAGE(fixtureMessage.c_str());
}

} // namespace

void setUp() {}
void tearDown() {}

void test_real_wifi_handler_responses_are_emitted_as_json_fixture() {
    emitFixture();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_real_wifi_handler_responses_are_emitted_as_json_fixture);
    return UNITY_END();
}
