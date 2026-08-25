#pragma once

#include <Arduino.h>

#include "battery_manager.h"
#include "display.h"
#include "settings.h"

class PowerLifecycle {
  public:
    virtual ~PowerLifecycle() = default;
    virtual void prepareForShutdown() = 0;
    virtual void resumeAfterAbortedShutdown() = 0;
};

class PowerModule {
  public:
    void begin(BatteryManager* batteryMgr, V1Display* disp, SettingsManager* settings);
    void setLifecycle(PowerLifecycle& lifecycle) { lifecycle_ = &lifecycle; }

    // Perform a graceful shutdown. Ignored before side effects in car builds.
    void performShutdown();

    // Log initial battery status after display init.
    void logStartupStatus();

    // Mark that we have seen real V1 data (arms auto power-off on disconnect).
    // No-op in car builds.
    void onV1DataReceived();

    // Notify connection changes to manage auto power-off timers. No-op in car builds.
    void onV1ConnectionChange(bool connected);

    // Notify ALP heartbeat-presence changes to manage auto power-off timers. No-op in car builds.
    void onAlpSignalChange(bool active);

    // Run periodic tasks. Car builds retain battery polling but disable shutdown paths.
    void process(unsigned long nowMs);

    // A critical-battery warning is a higher-priority presentation owner. The
    // main loop uses this to keep normal renders, previews, and touch settings
    // from painting over the warning while core runtime work continues.
    bool ownsDisplayPresentation() const { return criticalBatteryPresentationActive_; }

    // Returns true once after warning ownership ends or a shutdown hardware
    // tail aborts, so the caller can invalidate caches and restore the current
    // authoritative runtime/maintenance screen.
    bool consumeDisplayRestoreRequest();
    bool consumeDisplayBrightnessRestoreRequest();

#ifdef UNIT_TEST
    using ShutdownHandoffObserver = void (*)(void*);

    bool lowBatteryWarningShownForTest() const { return criticalBatteryPresentationActive_; }
    unsigned long autoPowerOffTimerStartForTest() const { return autoPowerOffTimerStart_; }
    bool autoPowerOffArmedForTest() const { return autoPowerOffArmed_; }
    void performShutdownRequestForTest() { performShutdownRequest(); }
    void setShutdownHandoffObserverForTest(ShutdownHandoffObserver observer, void* context) {
        shutdownHandoffObserverForTest_ = observer;
        shutdownHandoffObserverContextForTest_ = context;
    }
#endif

  private:
    void performShutdownRequest();
    void reevaluateAutoPowerOffTimer(const V1Settings& settings, unsigned long nowMs);
    void releaseCriticalBatteryPresentation();
    void requestDisplayRestore();

    BatteryManager* battery_ = nullptr;
    V1Display* display_ = nullptr;
    SettingsManager* settings_ = nullptr;
    PowerLifecycle* lifecycle_ = nullptr;
#ifdef UNIT_TEST
    ShutdownHandoffObserver shutdownHandoffObserverForTest_ = nullptr;
    void* shutdownHandoffObserverContextForTest_ = nullptr;
#endif

    bool criticalBatteryPresentationActive_ = false;
    bool displayRestorePending_ = false;
    bool displayBrightnessRestorePending_ = false;
    unsigned long criticalBatteryTime_ = 0;

    unsigned long autoPowerOffTimerStart_ = 0; // 0 = timer not running
    bool autoPowerOffArmed_ = false;
    bool v1SignalPresent_ = false;
    bool alpSignalPresent_ = false;
};
