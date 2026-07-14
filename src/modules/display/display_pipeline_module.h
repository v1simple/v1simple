#pragma once

#include <stdint.h>

#include "render_frame_composer.h"

// Forward declarations — full headers included in display_pipeline_module.cpp
class V1Display;
struct DisplayState;
class PacketParser;
enum class DisplayMode;

class SettingsManager;
class V1BLEClient;
class AlertPersistenceModule;
class VoiceModule;
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
    VoiceModule* voice = nullptr;
    SpeedMuteModule* speedMute = nullptr;
    QuietCoordinatorModule* quiet = nullptr;
    AlpRuntimeModule* alp = nullptr;
    AlpEventLatch* alpLatch = nullptr;
    SpeedSourceSelector* speedSelector = nullptr; // For stealth mode speed display
};

class DisplayPipelineModule {
  public:
    void begin(const DisplayPipelineDependencies& dependencies);

    // Process after a successful parser.parse(); expects parser state already updated.
    void handleParsed(uint32_t nowMs);
    // D2 blink-refresh tick: re-renders the current frame WITHOUT
    // alert-persistence side effects. Triggered by the orchestrator when no
    // parsed frame ran this loop but live blink sources are active. See
    // docs/ARROW_BEHAVIOR_ANALYSIS_20260429.md D2.
    void refreshBlinkTick(uint32_t nowMs);
    void restoreCurrentOwner(uint32_t nowMs);
    bool allowsObdPairGesture(uint32_t nowMs) const;

  private:
    DisplayMode* displayMode_ = nullptr;
    V1Display* display_ = nullptr;
    PacketParser* parser_ = nullptr;
    SettingsManager* settings_ = nullptr;
    V1BLEClient* ble_ = nullptr;
    AlertPersistenceModule* alertPersistence_ = nullptr;
    VoiceModule* voice_ = nullptr;
    SpeedMuteModule* speedMute_ = nullptr;
    QuietCoordinatorModule* quiet_ = nullptr;
    AlpRuntimeModule* alp_ = nullptr;
    AlpEventLatch* alpLatch_ = nullptr;
    SpeedSourceSelector* speedSelector_ = nullptr;
    RenderFrameComposer composer_;

    int lastPersistenceSlot_ = -1;
    // Display-owned alert presentation: retains best-known gun/direction across
    // brief abnormal LISTENING gaps and session-active TEARDOWN rearm gaps.
    AlpLaserEvent alpAlertPresentation_{};
    // Edge detector for presented active->inactive ALP transitions; begin()
    // resets it because the pipeline assumes synchronous ticks.
    bool lastPresentedAlpEventActive_ = false;

    // ALP display decision tracking — log only on transitions
    bool lastAlpDisplayTraceRelevant_ = false;
    char lastAlpDisplayLogEvent_[24] = "";
    char lastAlpDisplayLogDetail_[192] = "";
    uint32_t lastAlpDisplaySnapshotLogMs_ = 0;

    void logAlpDisplaySnapshot(uint32_t nowMs, const char* event, const char* detail, bool traceRelevant);
    RenderFrame buildRenderFrame(uint32_t nowMs, const V1Settings& settingsRef);
    RenderFrame buildDisconnectedRestoreFrame(uint32_t nowMs, const V1Settings& settingsRef);
    AlpLaserEvent buildPresentedAlpEvent(const AlpLaserEvent& rawAlpEvent, uint32_t nowMs);
    void updateAlpLatch(const AlpLaserEvent& alpEvent, uint32_t nowMs, uint8_t persistSec);
    void runVoice(const RenderFrame& frame, const V1Settings& settingsRef, uint32_t nowMs);
    void renderComposedFrame(uint32_t nowMs, const RenderFrame& frame, bool restoreContext, const char* logEvent,
                             bool forceRedraw = false);
    void recordPerfTiming(unsigned long startUs, unsigned long endUs);
};
