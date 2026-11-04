// Mock display.h for native unit testing
#pragma once
#ifndef DISPLAY_H
#define DISPLAY_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#ifdef ARDUINO
#include <Arduino.h>
#else
#include "Arduino.h"
#endif

// Full definition needed — AlertData is used as a value field below.
// packet_parser_types.h is pure data with no Arduino dependency.
#include "../../src/packet_parser_types.h"
#include "../../src/modules/alp/alp_laser_event.h"
#include "../../src/modules/alp/alp_runtime_module.h"
#include "../../include/display_ble_context.h"
#include "../../include/render_frame.h"
#include "../../include/display_presentation.h"

struct DisplayState;

enum class DisplayMockPresentation : uint8_t {
    NONE,
    SCANNING,
    RESTING,
    STEALTH,
    DISCONNECTED,
    MAINTENANCE,
    CONTENT,
    LOW_BATTERY,
    SHUTDOWN,
    CLEARED,
};

enum class DisplayMockOperation : uint8_t {
    NONE,
    SHOW_SCANNING,
    SHOW_RESTING,
    SHOW_DISCONNECTED,
    SHOW_MAINTENANCE,
    UPDATE_CONTENT,
    SHOW_LOW_BATTERY,
    SHOW_SHUTDOWN,
    RESET_CHANGE_TRACKING,
    CLEAR,
    FLUSH,
    SET_BRIGHTNESS,
};

struct DisplayMockLifecycleState {
    DisplayMockPresentation presentation = DisplayMockPresentation::NONE;
    DisplayMockOperation lastOperation = DisplayMockOperation::NONE;
    DisplayBleContext bleContext{};
    bool bleProxyEnabled = false;
    bool bleProxyConnected = false;
    bool bleReceiving = true;
    bool panelFrameBlack = false;
    uint8_t brightness = 0;
    uint32_t sequence = 0;
    uint32_t presentationSequence = 0;
    uint32_t resetChangeTrackingSequence = 0;
    uint32_t bleRefreshSequence = 0;
    uint32_t clearSequence = 0;
    uint32_t flushSequence = 0;
    uint32_t brightnessSequence = 0;
};

// Screen dimensions (needed by some tests)
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 640
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 172
#endif

/**
 * Mock V1Display - tracks method calls for verification
 */
class V1Display {
public:
    // Mirrors the blink-refresh API used by DisplayOrchestrationModule. Tests
    // can set lastBlinkToggleMs to simulate a stale or current phase.
    unsigned long lastBlinkToggleMs = 0;
    unsigned long getLastBlinkToggleMs() const { return lastBlinkToggleMs; }
    static constexpr unsigned long getBlinkIntervalMs() { return 96; }
    bool secondaryCardRefreshDue(uint32_t) const { return false; }
    bool isStealthScreen() const { return lifecycleState_.presentation == DisplayMockPresentation::STEALTH; }
    uint32_t renderSeq = 0;

    PresentationStatusSnapshot lastPresentationStatus{};
    CardPersistencePolicy lastCardPersistence = CardPersistencePolicy::LiveGrace;
    uint32_t lastPaintNowMs = 0;
    uint32_t lastProfileSelectedAtMs = 0;
    unsigned refreshBleIndicatorCalls = 0;
    unsigned refreshObdIndicatorCalls = 0;
    void refreshBleIndicator(const DisplayPaintContext& context) {
        ++refreshBleIndicatorCalls;
        lifecycleState_.bleRefreshSequence = ++lifecycleState_.sequence;
        recordContext(context);
    }
    void refreshObdIndicator(const DisplayPaintContext& context) {
        ++refreshObdIndicatorCalls;
        drawObdIndicator(context);
    }
    void drawConnectionIndicators(const DisplayPaintContext& context) {
        drawWiFiIndicator(context);
        drawBatteryIndicator(context);
    }
    // Call tracking
    int showScanningCalls = 0;
    int showRestingCalls = 0;
    int showDisconnectedCalls = 0;
    int setBrightnessCalls = 0;
    uint8_t lastBrightness = 0;
    uint32_t displayRecoverySequence = 0;
    uint32_t showDisconnectedSequence = 0;
    uint32_t flushSequence = 0;
    uint32_t setBrightnessSequence = 0;
    int showMaintenanceModeCalls = 0;
    char lastMaintenanceIp[24] = "";
    bool lastMaintenanceStationMode = false;
    int updateCalls = 0;
    int updatePersistedCalls = 0;
    int clearCalls = 0;
    int flushCalls = 0;
    int forceNextRedrawCalls = 0;
    int drawWiFiIndicatorCalls = 0;
    int drawObdIndicatorCalls = 0;
    int drawBatteryIndicatorCalls = 0;
    int drawProfileIndicatorCalls = 0;
    int lastProfileIndicatorSlot = -1;
    int showLowBatteryCalls = 0;
    int showShutdownCalls = 0;
    int setProfileIndicatorSlotCalls = 0;
    int lastProfileIndicatorSlotValue = -1;
    int flushRegionCalls = 0;
    int16_t lastFlushX = 0;
    int16_t lastFlushY = 0;
    int16_t lastFlushW = 0;
    int16_t lastFlushH = 0;
    int showSettingsSlidersCalls = 0;
    int updateSettingsSlidersCalls = 0;
    int hideBrightnessSliderCalls = 0;
    int lastSettingsBrightness = 0;
    int lastSettingsVolume = 0;
    int lastSettingsActiveSlider = -1;
    int activeSliderFromTouch = -1;
    int lastAlertUpdateCount = 0;
    DisplayState lastAlertDisplayState{};
    bool hasLastAlertDisplayState = false;
    int renderFrameCalls = 0;
    RenderFrame lastRenderFrame{};
    bool hasLastRenderFrame = false;
    DisplayMockLifecycleState lifecycleState_{};

    // resetChangeTracking tracking
    int resetChangeTrackingCalls = 0;

    void reset() {
        lastPresentationStatus = {};
        lastCardPersistence = CardPersistencePolicy::LiveGrace;
        lastPaintNowMs = lastProfileSelectedAtMs = 0;
        refreshBleIndicatorCalls = refreshObdIndicatorCalls = 0;
        showScanningCalls = 0;
        showRestingCalls = 0;
        showDisconnectedCalls = 0;
        setBrightnessCalls = 0;
        lastBrightness = 0;
        displayRecoverySequence = 0;
        showDisconnectedSequence = 0;
        flushSequence = 0;
        setBrightnessSequence = 0;
        showMaintenanceModeCalls = 0;
        lastMaintenanceIp[0] = '\0';
        lastMaintenanceStationMode = false;
        updateCalls = 0;
        updatePersistedCalls = 0;
        clearCalls = 0;
        flushCalls = 0;
        forceNextRedrawCalls = 0;
        drawWiFiIndicatorCalls = 0;
        drawObdIndicatorCalls = 0;
        drawBatteryIndicatorCalls = 0;
        drawProfileIndicatorCalls = 0;
        lastProfileIndicatorSlot = -1;
        showLowBatteryCalls = 0;
        showShutdownCalls = 0;
        setProfileIndicatorSlotCalls = 0;
        lastProfileIndicatorSlotValue = -1;
        flushRegionCalls = 0;
        lastFlushX = 0;
        lastFlushY = 0;
        lastFlushW = 0;
        lastFlushH = 0;
        showSettingsSlidersCalls = 0;
        updateSettingsSlidersCalls = 0;
        hideBrightnessSliderCalls = 0;
        lastSettingsBrightness = 0;
        lastSettingsVolume = 0;
        lastSettingsActiveSlider = -1;
        activeSliderFromTouch = -1;
        lastAlertUpdateCount = 0;
        lastAlertDisplayState = DisplayState{};
        hasLastAlertDisplayState = false;
        renderFrameCalls = 0;
        lastRenderFrame = RenderFrame{};
        hasLastRenderFrame = false;
        lifecycleState_ = DisplayMockLifecycleState{};
        lastPriorityAlert = AlertData{};
        hasLastPriorityAlert = false;
        resetChangeTrackingCalls = 0;
        renderSeq = 0;
    }

    // Display methods
    void showScanning(const DisplayPaintContext& context, bool = true) {
        recordContext(context);
        showScanningCalls++;
        recordPresentation(DisplayMockPresentation::SCANNING, DisplayMockOperation::SHOW_SCANNING);
    }
    void showResting(const DisplayPaintContext& context) {
        recordContext(context);
        showRestingCalls++;
        recordPresentation(DisplayMockPresentation::RESTING, DisplayMockOperation::SHOW_RESTING);
    }
    void showDisconnected(const DisplayPaintContext& context) {
        recordContext(context);
        showDisconnectedCalls++;
        showDisconnectedSequence = ++displayRecoverySequence;
        recordPresentation(DisplayMockPresentation::DISCONNECTED, DisplayMockOperation::SHOW_DISCONNECTED);
    }
    void showMaintenanceMode(const DisplayPaintContext& context, const char* ip = nullptr, bool stationMode = false) {
        recordContext(context);
        showMaintenanceModeCalls++;
        if (ip != nullptr) {
            std::strncpy(lastMaintenanceIp, ip, sizeof(lastMaintenanceIp) - 1);
            lastMaintenanceIp[sizeof(lastMaintenanceIp) - 1] = '\0';
        } else {
            lastMaintenanceIp[0] = '\0';
        }
        lastMaintenanceStationMode = stationMode;
        recordPresentation(DisplayMockPresentation::MAINTENANCE, DisplayMockOperation::SHOW_MAINTENANCE);
    }
    
    void update(const DisplayState& /*state*/) {
        updateCalls++;
        renderSeq++;
        recordPresentation(DisplayMockPresentation::CONTENT, DisplayMockOperation::UPDATE_CONTENT);
    }
    void update(const AlertData& priority, const AlertData* /*alerts*/,
                int count, const DisplayState& state) {
        updateCalls++;
        renderSeq++;
        lastAlertUpdateCount = count;
        lastPriorityAlert = priority;
        hasLastPriorityAlert = true;
        lastAlertDisplayState = state;
        hasLastAlertDisplayState = true;
        recordPresentation(DisplayMockPresentation::CONTENT, DisplayMockOperation::UPDATE_CONTENT);
    }

    AlertData lastPriorityAlert{};
    bool hasLastPriorityAlert = false;
    void updatePersisted(const AlertData& /*alert*/, const DisplayState& /*state*/) {
        updatePersistedCalls++;
        renderSeq++;
        recordPresentation(DisplayMockPresentation::CONTENT, DisplayMockOperation::UPDATE_CONTENT);
    }

    void renderFrame(const RenderFrame& frame, const DisplayPaintContext& context, int visibleCardCount = -1) {
        recordContext(context);
        const int cardCount = visibleCardCount < 0 || visibleCardCount > frame.cardCount ? frame.cardCount : visibleCardCount;
        renderFrameCalls++;
        lastRenderFrame = frame;
        lastRenderFrame.cardCount = cardCount;
        hasLastRenderFrame = true;

        switch (frame.primaryKind) {
            case RenderFramePrimaryKind::NONE:
                return;

            case RenderFramePrimaryKind::IDLE:
                if (frame.stealthMode) {
                    recordPresentation(DisplayMockPresentation::STEALTH, DisplayMockOperation::UPDATE_CONTENT);
                } else {
                    update(frame.primaryState);
                }
                return;

            case RenderFramePrimaryKind::V1_LIVE:
                update(frame.v1Priority, nullptr, cardCount, frame.primaryState);
                return;

            case RenderFramePrimaryKind::V1_PERSISTED:
                updatePersisted(frame.v1Priority, frame.primaryState);
                return;

            case RenderFramePrimaryKind::ALP_LIVE:
            case RenderFramePrimaryKind::ALP_PERSISTED: {
                AlertData syntheticAlert{};
                syntheticAlert.isValid = true;
                syntheticAlert.band = BAND_LASER;
                syntheticAlert.frequency = 0;
                switch (frame.alpPrimary.direction) {
                    case AlpLaserDirection::FRONT:
                        syntheticAlert.direction = DIR_FRONT;
                        break;

                    case AlpLaserDirection::REAR:
                        syntheticAlert.direction = DIR_REAR;
                        break;

                    case AlpLaserDirection::UNKNOWN:
                    default:
                        syntheticAlert.direction = DIR_NONE;
                        break;
                }
                syntheticAlert.frontStrength = 6;
                update(syntheticAlert, nullptr, cardCount, frame.primaryState);
                return;
            }
        }
    }
    
    void clear(const DisplayPaintContext& context) {
        recordContext(context);
        clearCalls++;
        renderSeq++;
        recordOperation(DisplayMockOperation::CLEAR);
        lifecycleState_.presentation = DisplayMockPresentation::CLEARED;
        lifecycleState_.presentationSequence = lifecycleState_.sequence;
        lifecycleState_.panelFrameBlack = true;
        lifecycleState_.clearSequence = lifecycleState_.sequence;
    }
    void flush() {
        flushCalls++;
        renderSeq++;
        flushSequence = ++displayRecoverySequence;
        recordOperation(DisplayMockOperation::FLUSH);
        lifecycleState_.flushSequence = lifecycleState_.sequence;
    }
    void forceNextRedraw() { forceNextRedrawCalls++; }
    
    void drawWiFiIndicator(const DisplayPaintContext& context) {
        recordContext(context); drawWiFiIndicatorCalls++; }
    void drawObdIndicator(const DisplayPaintContext& context) {
        recordContext(context); drawObdIndicatorCalls++; }
    void drawBatteryIndicator(const DisplayPaintContext& context) {
        recordContext(context); drawBatteryIndicatorCalls++; }
    void showLowBattery(const DisplayPaintContext& context) {
        recordContext(context);
        showLowBatteryCalls++;
        recordPresentation(DisplayMockPresentation::LOW_BATTERY, DisplayMockOperation::SHOW_LOW_BATTERY);
    }
    void showShutdown(const DisplayPaintContext& context) {
        recordContext(context);
        showShutdownCalls++;
        recordPresentation(DisplayMockPresentation::SHUTDOWN, DisplayMockOperation::SHOW_SHUTDOWN);
    }
    void drawProfileIndicator(const DisplayPaintContext& context) {
        recordContext(context);
        const int slot = lastProfileIndicatorSlotValue;
        drawProfileIndicatorCalls++;
        lastProfileIndicatorSlot = slot;
    }

    void setProfileIndicatorSlot(int slot, uint32_t selectedAtMs) {
        lastProfileSelectedAtMs = selectedAtMs;
        setProfileIndicatorSlotCalls++;
        lastProfileIndicatorSlotValue = slot;
    }

    void flushRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
        flushRegionCalls++;
        renderSeq++;
        lastFlushX = x;
        lastFlushY = y;
        lastFlushW = w;
        lastFlushH = h;
    }

    void setBrightness(uint8_t level) {
        setBrightnessCalls++;
        lastBrightness = level;
        setBrightnessSequence = ++displayRecoverySequence;
        recordOperation(DisplayMockOperation::SET_BRIGHTNESS);
        lifecycleState_.brightness = level;
        lifecycleState_.brightnessSequence = lifecycleState_.sequence;
    }
    void showSettingsSliders(const DisplayPaintContext& context, uint8_t brightnessLevel, uint8_t volumeLevel) {
        recordContext(context);
        showSettingsSlidersCalls++;
        lastSettingsBrightness = brightnessLevel;
        lastSettingsVolume = volumeLevel;
    }
    void updateSettingsSliders(const DisplayPaintContext& context, uint8_t brightnessLevel, uint8_t volumeLevel, int activeSlider) {
        recordContext(context);
        updateSettingsSlidersCalls++;
        lastSettingsBrightness = brightnessLevel;
        lastSettingsVolume = volumeLevel;
        lastSettingsActiveSlider = activeSlider;
    }
    void hideBrightnessSlider(const DisplayPaintContext& context) {
        recordContext(context); hideBrightnessSliderCalls++; }
    int getActiveSliderFromTouch(int16_t /*touchY*/) { return activeSliderFromTouch; }
    
    void resetChangeTracking() {
        resetChangeTrackingCalls++;
        recordOperation(DisplayMockOperation::RESET_CHANGE_TRACKING);
        lifecycleState_.resetChangeTrackingSequence = lifecycleState_.sequence;
    }
    const DisplayMockLifecycleState& lifecycleState() const { return lifecycleState_; }
  private:
    void recordContext(const DisplayPaintContext& context) {
        lastPresentationStatus = context.status;
        lastCardPersistence = context.cardPersistence;
        lastPaintNowMs = context.nowMs;
        lifecycleState_.bleContext = context.status.ble;
        lifecycleState_.bleProxyEnabled = context.status.bleProxyEnabled;
        lifecycleState_.bleProxyConnected = context.status.bleProxyClientConnected;
        lifecycleState_.bleReceiving = context.status.bleReceiving;
    }
    void recordOperation(DisplayMockOperation operation) {
        lifecycleState_.lastOperation = operation;
        ++lifecycleState_.sequence;
    }

    void recordPresentation(DisplayMockPresentation presentation, DisplayMockOperation operation) {
        recordOperation(operation);
        lifecycleState_.presentation = presentation;
        lifecycleState_.presentationSequence = lifecycleState_.sequence;
        lifecycleState_.panelFrameBlack = false;
    }
};

#endif  // DISPLAY_H
