// Touch UI module - handles BOOT button brightness/volume adjustment UI,
// maintenance reboot entry, and OBD manual-pair gesture.

#pragma once

#include <Arduino.h>
#include "display.h"
#include "modules/obd/obd_runtime_module.h"
#include "settings.h"
#include "touch_handler.h"

class TouchUiModule {
public:
    TouchUiModule() = default;

    struct Callbacks {
        bool (*isWifiSetupActive)(void* ctx) = nullptr;
        void* isWifiSetupActiveCtx = nullptr;
        void (*stopWifiSetup)(void* ctx) = nullptr;       // stop AP/setup mode if unexpectedly active
        void* stopWifiSetupCtx = nullptr;
        void (*requestMaintenanceBoot)(void* ctx) = nullptr;  // persist flag + reboot
        void* requestMaintenanceBootCtx = nullptr;
        void (*drawWifiIndicator)(void* ctx) = nullptr;
        void* drawWifiIndicatorCtx = nullptr;
        void (*restoreDisplay)(void* ctx) = nullptr;      // refresh display with current state
        void* restoreDisplayCtx = nullptr;
        ObdRuntimeStatus (*readObdStatus)(uint32_t nowMs, void* ctx) = nullptr;
        void* readObdStatusCtx = nullptr;
        bool (*requestObdManualPairScan)(uint32_t nowMs, void* ctx) = nullptr;
        void* requestObdManualPairScanCtx = nullptr;
        bool (*isObdPairGestureSafe)(uint32_t nowMs, void* ctx) = nullptr;
        void* isObdPairGestureSafeCtx = nullptr;
    };

    void begin(V1Display* disp,
               TouchHandler* touch,
               SettingsManager* settings,
               const Callbacks& cbs);

    // Returns true if UI consumed the loop (brightness/volume adjustment active)
    bool process(unsigned long nowMs, bool bootPressed);

private:
    void enterAdjustMode();
    void exitAdjustModeAndSave();
    bool handleSliderTouch(unsigned long nowMs);
    bool canArmObdPairGesture(unsigned long nowMs) const;
    void updateObdIndicatorAttention(bool attention, unsigned long nowMs);

    // State
    V1Display* display_ = nullptr;
    TouchHandler* touchHandler_ = nullptr;
    SettingsManager* settings_ = nullptr;
    Callbacks callbacks_{};

    bool brightnessAdjustMode_ = false;
    uint8_t brightnessAdjustValue_ = 200;
    uint8_t volumeAdjustValue_ = 75;
    int activeSlider_ = 0;
    unsigned long lastVolumeChangeMs_ = 0;
    unsigned long lastSliderRedrawMs_ = 0;

    unsigned long bootPressStart_ = 0;
    bool bootWasPressed_ = false;
    bool obdPairGestureArmed_ = false;
    unsigned long lastShortPressReleaseMs_ = 0;

    // Timing constants (mirrors previous inline logic)
    static constexpr unsigned long BOOT_DEBOUNCE_MS = 300;
    static constexpr unsigned long MAINTENANCE_BOOT_LONG_PRESS_MS = 4000;
    static constexpr unsigned long OBD_PAIR_LONG_PRESS_MS = 10000;
    static constexpr unsigned long DOUBLE_PRESS_WINDOW_MS = 600;
    static constexpr unsigned long VOLUME_TEST_DEBOUNCE_MS = 1000;
    static constexpr unsigned long SLIDER_REDRAW_MIN_MS = 50;  // Cap slider redraw rate (~20 Hz)
};
