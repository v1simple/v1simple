#pragma once

#include <Arduino.h>
#include "display_ble_context.h"

class V1Display;
class V1BLEClient;
class DisplayPreviewModule;
class DisplayRestoreModule;
class PacketParser;
class VolumeFadeModule;
class SpeedMuteModule;
class QuietCoordinatorModule;

struct DisplayOrchestrationEarlyContext {
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

struct DisplayOrchestrationRefreshContext {
    uint32_t nowMs = 0;
    bool bootSplashHoldActive = false;
    bool overloadLateThisLoop = false;
    bool pipelineRanThisLoop = false;
    bool v1PersistenceRefreshDue = false;
};

class DisplayOrchestrationModule {
  public:
    void begin(V1Display* displayPtr, V1BLEClient* bleClient, DisplayPreviewModule* previewModule,
               DisplayRestoreModule* restoreModule, PacketParser* parserPtr,
               VolumeFadeModule* volumeFadeModule, SpeedMuteModule* speedMuteModule,
               QuietCoordinatorModule* quietCoordinator);

    void processEarly(const DisplayOrchestrationEarlyContext& ctx);
    bool processParsedFrame(const DisplayOrchestrationParsedContext& ctx);
    // True when a blink or V1 persistence expiry needs a render this loop.
    bool processLightweightRefresh(const DisplayOrchestrationRefreshContext& ctx);

  private:
    void syncQuietPresentation();
    void executeVolumeFade(uint32_t nowMs);
    bool processSpeedVolume(uint32_t nowMs);

    V1Display* display_ = nullptr;
    V1BLEClient* ble_ = nullptr;
    DisplayPreviewModule* preview_ = nullptr;
    DisplayRestoreModule* restore_ = nullptr;
    PacketParser* parser_ = nullptr;
    VolumeFadeModule* volumeFade_ = nullptr;
    SpeedMuteModule* speedMute_ = nullptr;
    QuietCoordinatorModule* quiet_ = nullptr;

    // Blink refresh cadence is derived from the renderer's phase timestamp.
};
