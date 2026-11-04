#pragma once

#include <stdint.h>

#include "render_frame_composer.h"

// Forward declarations — full headers included in display_pipeline_module.cpp
class V1Display;
struct DisplayState;
struct V1Settings;
class PacketParser;
enum class DisplayMode;

class SettingsManager;
class V1BLEClient;
class AlertPersistenceModule;
class SpeedMuteModule;
class QuietCoordinatorModule;
class AlpRuntimeModule;
class AlpEventLatch;
class SpeedSourceSelector;

struct DisplayPipelineDependencies {
    DisplayMode* displayMode = nullptr;
    V1Display* display = nullptr;
    PacketParser* parser = nullptr;
    SettingsManager* settings = nullptr;
    V1BLEClient* ble = nullptr;
    AlertPersistenceModule* alertPersistence = nullptr;
    SpeedMuteModule* speedMute = nullptr;
    QuietCoordinatorModule* quiet = nullptr;
    AlpRuntimeModule* alp = nullptr;
    AlpEventLatch* alpLatch = nullptr;
    SpeedSourceSelector* speedSelector = nullptr; // For stealth mode speed display
};

class DisplayPipelineModule {
  public:
    void begin(const DisplayPipelineDependencies& dependencies);

    // Compose once at the admitted parsed position; voice and paint consume
    // this same filtered frame. Painting samples its own current entry time.
    bool prepareParsed(uint32_t nowMs, RenderFrame& out);
    void presentPrepared(const RenderFrame& frame, uint32_t paintNowMs);
    // Re-renders the current owner without voice side effects when a blink or
    // V1 persistence deadline needs a display update.
    void refreshBlinkTick(uint32_t nowMs);
    bool restoreCurrentOwner(uint32_t nowMs);
    bool allowsObdPairGesture(uint32_t nowMs) const;
    bool consumeAlpPresentationRefreshDue(uint32_t nowMs);
    bool v1PersistenceRefreshDue(uint32_t nowMs) const;

  private:
    DisplayMode* displayMode_ = nullptr;
    V1Display* display_ = nullptr;
    PacketParser* parser_ = nullptr;
    SettingsManager* settings_ = nullptr;
    V1BLEClient* ble_ = nullptr;
    AlertPersistenceModule* alertPersistence_ = nullptr;
    SpeedMuteModule* speedMute_ = nullptr;
    QuietCoordinatorModule* quiet_ = nullptr;
    AlpRuntimeModule* alp_ = nullptr;
    AlpEventLatch* alpLatch_ = nullptr;
    SpeedSourceSelector* speedSelector_ = nullptr;
    RenderFrameComposer composer_;
    bool scanningPresented_ = false;

    int lastPersistenceSlot_ = -1;
    // Display-owned alert presentation: retains best-known gun/direction across
    // brief abnormal LISTENING gaps and session-active TEARDOWN rearm gaps.
    AlpLaserEvent alpAlertPresentation_{};
    // Edge detector for presented active->inactive ALP transitions; begin()
    // resets it because the pipeline assumes synchronous ticks.
    bool lastPresentedAlpEventActive_ = false;
    uint32_t alpHoldRefreshDeadlineMs_ = 0;
    uint32_t alpPersistRefreshDeadlineMs_ = 0;
    uint32_t v1PersistRefreshDeadlineMs_ = 0;
    uint32_t lastV1ClearRevision_ = 0;
    uint32_t lastAlpEndRevision_ = 0;
    RenderFrame buildRenderFrame(uint32_t nowMs, const V1Settings& settingsRef);
    RenderFrame buildDisconnectedFrame(uint32_t nowMs);
    AlpLaserEvent buildPresentedAlpEvent(const AlpLaserEvent& rawAlpEvent, uint32_t nowMs);
    void updateAlpLatch(const AlpLaserEvent& alpEvent, uint32_t nowMs, uint8_t persistSec);
    void renderComposedFrame(uint32_t nowMs, const RenderFrame& frame, bool forceRedraw = false);
};
