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

struct DisplayState;

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
    // D2 fix: blink-toggle timestamp accessor + interval, mirroring the real
    // V1Display API used by DisplayOrchestrationModule for blink-refresh
    // gating. Tests can set lastBlinkToggleMs to simulate stale/fresh phase.
    unsigned long lastBlinkToggleMs = 0;
    unsigned long getLastBlinkToggleMs() const { return lastBlinkToggleMs; }
    static constexpr unsigned long getBlinkIntervalMs() { return 96; }
    static constexpr uint16_t rawFramebufferWidth() { return 172; }
    static constexpr uint16_t rawFramebufferHeight() { return 640; }
    static constexpr size_t kRawFramebufferPixels = 172u * 640u;
    static constexpr uint16_t logicalFramebufferWidth() { return SCREEN_WIDTH; }
    static constexpr uint16_t logicalFramebufferHeight() { return SCREEN_HEIGHT; }
    static constexpr const char* rawFramebufferFormat() { return "RGB565LE"; }
    static constexpr const char* rawFramebufferTransform() { return "canvas-rotation-1"; }
    uint16_t framebuffer[kRawFramebufferPixels] = {};
    uint32_t renderSeq = 0;
    bool framebufferAvailable = true;

    // Call tracking
    int showScanningCalls = 0;
    int showRestingCalls = 0;
    int showDisconnectedCalls = 0;
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
    int setSpeedVolZeroActiveCalls = 0;
    bool lastSpeedVolZeroActiveValue = false;
    int setBleContextCalls = 0;
    DisplayBleContext lastBleContext{};
    int setBLEProxyStatusCalls = 0;
    bool lastBleProxyEnabled = false;
    bool lastBleProxyConnected = false;
    bool lastBleReceiving = true;
    int setPreviewIndicatorOverridesActiveCalls = 0;
    bool lastPreviewIndicatorOverridesActive = false;
    int setAlpPreviewStateCalls = 0;
    bool lastAlpPreviewEnabled = false;
    uint8_t lastAlpPreviewState = 0;
    uint8_t lastAlpPreviewHbByte1 = 0;
    int setObdPreviewStateCalls = 0;
    bool lastObdPreviewEnabled = false;
    bool lastObdPreviewConnected = false;
    bool lastObdPreviewScanAttention = false;
    int setProfileIndicatorSlotCalls = 0;
    int lastProfileIndicatorSlotValue = -1;
    int setObdStatusCalls = 0;
    bool lastObdEnabled = false;
    bool lastObdConnected = false;
    bool lastObdScanAttention = false;
    int setObdAttentionCalls = 0;
    bool lastObdAttention = false;
    int syncTopIndicatorsCalls = 0;
    uint32_t lastSyncTopIndicatorsNowMs = 0;
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

    // resetChangeTracking tracking
    int resetChangeTrackingCalls = 0;

    void reset() {
        showScanningCalls = 0;
        showRestingCalls = 0;
        showDisconnectedCalls = 0;
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
        setSpeedVolZeroActiveCalls = 0;
        lastSpeedVolZeroActiveValue = false;
        setBleContextCalls = 0;
        lastBleContext = DisplayBleContext{};
        setBLEProxyStatusCalls = 0;
        lastBleProxyEnabled = false;
        lastBleProxyConnected = false;
        lastBleReceiving = true;
        setPreviewIndicatorOverridesActiveCalls = 0;
        lastPreviewIndicatorOverridesActive = false;
        setAlpPreviewStateCalls = 0;
        lastAlpPreviewEnabled = false;
        lastAlpPreviewState = 0;
        lastAlpPreviewHbByte1 = 0;
        setObdPreviewStateCalls = 0;
        lastObdPreviewEnabled = false;
        lastObdPreviewConnected = false;
        lastObdPreviewScanAttention = false;
        setProfileIndicatorSlotCalls = 0;
        lastProfileIndicatorSlotValue = -1;
        setObdStatusCalls = 0;
        lastObdEnabled = false;
        lastObdConnected = false;
        lastObdScanAttention = false;
        setObdAttentionCalls = 0;
        lastObdAttention = false;
        syncTopIndicatorsCalls = 0;
        lastSyncTopIndicatorsNowMs = 0;
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
        lastPriorityAlert = AlertData{};
        hasLastPriorityAlert = false;
        setAlpFrequencyOverrideCalls = 0;
        clearAlpFrequencyOverrideCalls = 0;
        lastAlpFreqOverride[0] = '\0';
        setAlpLaserEventCalls = 0;
        resetChangeTrackingCalls = 0;
        renderSeq = 0;
        framebufferAvailable = true;
        std::memset(framebuffer, 0, sizeof(framebuffer));
        setVisualTestBlinkPhaseCalls = 0;
        lastBlinkPhase = true;
        enableVisualFlushShadowCalls = 0;
        disableVisualFlushShadowCalls = 0;
        flushShadowEnabled = false;
        flushShadowAllocFails = false;
    }

    // Display methods
    void showScanning() { showScanningCalls++; }
    void showResting() { showRestingCalls++; }
    void showDisconnected() { showDisconnectedCalls++; }
    void showMaintenanceMode(const char* ip = nullptr, bool stationMode = false) {
        showMaintenanceModeCalls++;
        if (ip != nullptr) {
            std::strncpy(lastMaintenanceIp, ip, sizeof(lastMaintenanceIp) - 1);
            lastMaintenanceIp[sizeof(lastMaintenanceIp) - 1] = '\0';
        } else {
            lastMaintenanceIp[0] = '\0';
        }
        lastMaintenanceStationMode = stationMode;
    }
    
    void update(const DisplayState& /*state*/) { updateCalls++; renderSeq++; }
    void update(const AlertData& priority, const AlertData* /*alerts*/,
                int count, const DisplayState& state) {
        updateCalls++;
        renderSeq++;
        lastAlertUpdateCount = count;
        lastPriorityAlert = priority;
        hasLastPriorityAlert = true;
        lastAlertDisplayState = state;
        hasLastAlertDisplayState = true;
    }

    AlertData lastPriorityAlert{};
    bool hasLastPriorityAlert = false;
    void updatePersisted(const AlertData& /*alert*/, const DisplayState& /*state*/) { 
        updatePersistedCalls++; 
        renderSeq++;
    }

    void renderFrame(const RenderFrame& frame) {
        renderFrameCalls++;
        lastRenderFrame = frame;
        hasLastRenderFrame = true;

        switch (frame.primaryKind) {
            case RenderFramePrimaryKind::NONE:
                return;

            case RenderFramePrimaryKind::IDLE:
                update(frame.primaryState);
                return;

            case RenderFramePrimaryKind::V1_LIVE:
                update(frame.v1Priority, nullptr, frame.cardCount, frame.primaryState);
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
                update(syntheticAlert, nullptr, frame.cardCount, frame.primaryState);
                return;
            }
        }
    }
    
    void clear() { clearCalls++; renderSeq++; }
    void flush() { flushCalls++; renderSeq++; }
    void forceNextRedraw() { forceNextRedrawCalls++; }
    
    void drawWiFiIndicator() { drawWiFiIndicatorCalls++; }
    void drawObdIndicator() { drawObdIndicatorCalls++; }
    void drawBatteryIndicator() { drawBatteryIndicatorCalls++; }
    void showLowBattery() { showLowBatteryCalls++; }
    void showShutdown() { showShutdownCalls++; }
    void drawProfileIndicator(int slot) {
        drawProfileIndicatorCalls++;
        lastProfileIndicatorSlot = slot;
    }
    
    void setSpeedVolZeroActive(bool active) {
        setSpeedVolZeroActiveCalls++;
        lastSpeedVolZeroActiveValue = active;
    }
    
    void setBleContext(const DisplayBleContext& ctx) {
        setBleContextCalls++;
        lastBleContext = ctx;
    }

    void setBLEProxyStatus(bool proxyEnabled, bool proxyConnected, bool receiving = true) {
        setBLEProxyStatusCalls++;
        lastBleProxyEnabled = proxyEnabled;
        lastBleProxyConnected = proxyConnected;
        lastBleReceiving = receiving;
    }

    void setPreviewIndicatorOverridesActive(bool active) {
        setPreviewIndicatorOverridesActiveCalls++;
        lastPreviewIndicatorOverridesActive = active;
    }

    void setAlpPreviewState(bool enabled, uint8_t state, uint8_t hbByte1) {
        setAlpPreviewStateCalls++;
        lastAlpPreviewEnabled = enabled;
        lastAlpPreviewState = state;
        lastAlpPreviewHbByte1 = hbByte1;
    }

    void setObdPreviewState(bool enabled, bool connected, bool scanAttention) {
        setObdPreviewStateCalls++;
        lastObdPreviewEnabled = enabled;
        lastObdPreviewConnected = connected;
        lastObdPreviewScanAttention = scanAttention;
    }

    void setProfileIndicatorSlot(int slot) {
        setProfileIndicatorSlotCalls++;
        lastProfileIndicatorSlotValue = slot;
    }

    void setObdStatus(bool enabled, bool connected, bool scanAttention = false) {
        setObdStatusCalls++;
        lastObdEnabled = enabled;
        lastObdConnected = connected;
        lastObdScanAttention = scanAttention;
    }

    void setObdAttention(bool attention) {
        setObdAttentionCalls++;
        lastObdAttention = attention;
    }

    void refreshObdIndicator(uint32_t nowMs) {
        syncTopIndicators(nowMs);
        drawObdIndicator();
    }

    void syncTopIndicators(uint32_t nowMs) {
        syncTopIndicatorsCalls++;
        lastSyncTopIndicatorsNowMs = nowMs;
    }

    void flushRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
        flushRegionCalls++;
        renderSeq++;
        lastFlushX = x;
        lastFlushY = y;
        lastFlushW = w;
        lastFlushH = h;
    }

    // ALP frequency override
    int setAlpFrequencyOverrideCalls = 0;
    int clearAlpFrequencyOverrideCalls = 0;
    char lastAlpFreqOverride[16] = "";
    void setAlpFrequencyOverride(const char* text, bool /*defenseMode*/ = true) {
        setAlpFrequencyOverrideCalls++;
        strncpy(lastAlpFreqOverride, text, sizeof(lastAlpFreqOverride));
        lastAlpFreqOverride[sizeof(lastAlpFreqOverride) - 1] = '\0';
    }
    void clearAlpFrequencyOverride() { clearAlpFrequencyOverrideCalls++; }

    // ALP laser event (Phase 2 atomic setter)
    int setAlpLaserEventCalls = 0;
    AlpLaserEvent lastAlpLaserEvent{};
    void setAlpLaserEvent(const AlpLaserEvent& ev) {
        ++setAlpLaserEventCalls;
        lastAlpLaserEvent = ev;
    }

    void setBrightness(uint8_t /*level*/) {}
    void showSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel) {
        showSettingsSlidersCalls++;
        lastSettingsBrightness = brightnessLevel;
        lastSettingsVolume = volumeLevel;
    }
    void updateSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel, int activeSlider) {
        updateSettingsSlidersCalls++;
        lastSettingsBrightness = brightnessLevel;
        lastSettingsVolume = volumeLevel;
        lastSettingsActiveSlider = activeSlider;
    }
    void hideBrightnessSlider() { hideBrightnessSliderCalls++; }
    int getActiveSliderFromTouch(int16_t /*touchY*/) { return activeSliderFromTouch; }
    
    void resetChangeTracking() { resetChangeTrackingCalls++; }
    uint32_t renderSequenceId() const { return renderSeq; }
    void setVisualTestBlinkPhase(bool phase, unsigned long epochMs) {
        lastBlinkPhase = phase;
        lastBlinkToggleMs = epochMs;
        setVisualTestBlinkPhaseCalls++;
    }
    const uint8_t* rawFramebufferBytes() const {
        return framebufferAvailable ? reinterpret_cast<const uint8_t*>(framebuffer) : nullptr;
    }
    size_t rawFramebufferByteLength() const { return sizeof(framebuffer); }
    bool rawFramebufferAvailable() const { return framebufferAvailable; }
    bool enableVisualFlushShadow() {
        enableVisualFlushShadowCalls++;
        if (flushShadowAllocFails) {
            return false;
        }
        flushShadowEnabled = true;
        return true;
    }
    void disableVisualFlushShadow() {
        disableVisualFlushShadowCalls++;
        flushShadowEnabled = false;
    }
    bool flushShadowAvailable() const { return flushShadowEnabled; }
    const uint8_t* flushShadowBytes() const {
        // Mock semantics: the shadow mirrors the framebuffer; route tests
        // only need availability gating and byte-length agreement.
        return flushShadowEnabled ? reinterpret_cast<const uint8_t*>(framebuffer) : nullptr;
    }
    int setVisualTestBlinkPhaseCalls = 0;
    bool lastBlinkPhase = true;
    int enableVisualFlushShadowCalls = 0;
    int disableVisualFlushShadowCalls = 0;
    bool flushShadowEnabled = false;
    bool flushShadowAllocFails = false;
};

#endif  // DISPLAY_H
