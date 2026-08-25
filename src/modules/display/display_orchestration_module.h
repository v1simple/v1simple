#pragma once

#include <Arduino.h>
#include "display_ble_context.h"

class V1Display;
class V1BLEClient;
class BleQueueModule;
class DisplayPreviewModule;
class DisplayRestoreModule;
class PacketParser;
class SettingsManager;
class VolumeFadeModule;
class SpeedMuteModule;
class QuietCoordinatorModule;
class DisplayPipelineModule;

struct DisplayOrchestrationEarlyContext {
    uint32_t nowMs = 0;
    bool bootSplashHoldActive = false;
    bool overloadThisLoop = false;
    DisplayBleContext bleContext{};
    bool bleReceiving = false;
};

struct DisplayOrchestrationParsedContext {
    uint32_t nowMs = 0;
    bool parsedReady = false;
    bool bootSplashHoldActive = false;
};

struct DisplayOrchestrationParsedResult {
    bool runDisplayPipeline = false;
};

struct DisplayOrchestrationRefreshContext {
    uint32_t nowMs = 0;
    bool bootSplashHoldActive = false;
    bool overloadLateThisLoop = false;
    bool pipelineRanThisLoop = false;
};

struct DisplayOrchestrationRefreshResult {
    // When true, LoopDisplayModule should call the blink-refresh provider this
    // loop. Set by processLightweightRefresh when V1 has live blink sources
    // active and no parsed frame ran. Throttled internally to ~80 ms so the
    // renderer's 96 ms BLINK toggle has a fresh chance each period.
    bool runBlinkRefresh = false;
};

class DisplayOrchestrationModule {
  public:
    // Intentional wide wiring surface: this coordinator sits at the center of
    // display-pipeline handoff, so the cross-module dependencies stay explicit.
    void begin(V1Display* displayPtr, V1BLEClient* bleClient, BleQueueModule* bleQueueModule,
               DisplayPreviewModule* previewModule, DisplayRestoreModule* restoreModule, PacketParser* parserPtr,
               SettingsManager* settings, VolumeFadeModule* volumeFadeModule, SpeedMuteModule* speedMuteModule,
               QuietCoordinatorModule* quietCoordinator, DisplayPipelineModule* displayPipelineModule);

    void processEarly(const DisplayOrchestrationEarlyContext& ctx);
    DisplayOrchestrationParsedResult processParsedFrame(const DisplayOrchestrationParsedContext& ctx);
    DisplayOrchestrationRefreshResult processLightweightRefresh(const DisplayOrchestrationRefreshContext& ctx);

  private:
    void syncQuietPresentation();
    void executeVolumeFade(uint32_t nowMs);
    bool processSpeedVolume(uint32_t nowMs);

    V1Display* display_ = nullptr;
    V1BLEClient* ble_ = nullptr;
    BleQueueModule* bleQueue_ = nullptr;
    DisplayPreviewModule* preview_ = nullptr;
    DisplayRestoreModule* restore_ = nullptr;
    PacketParser* parser_ = nullptr;
    SettingsManager* settings_ = nullptr;
    VolumeFadeModule* volumeFade_ = nullptr;
    SpeedMuteModule* speedMute_ = nullptr;
    QuietCoordinatorModule* quiet_ = nullptr;
    DisplayPipelineModule* displayPipeline_ = nullptr;

    // Blink refresh cadence is derived from the renderer's phase timestamp.
};
