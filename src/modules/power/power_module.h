#pragma once

#include <Arduino.h>

#include "battery_manager.h"
#include "display.h"
#include "settings.h"

class PowerModule {
  public:
    using ShutdownPreparationCallback = void (*)(void*);
    using ShutdownAbortCallback = void (*)(void*);

    void begin(BatteryManager* batteryMgr, V1Display* disp, SettingsManager* settings);

    void setShutdownPreparationCallback(ShutdownPreparationCallback callback, void* context);
    void setShutdownAbortCallback(ShutdownAbortCallback callback, void* context);

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

#ifdef UNIT_TEST
    using ShutdownHandoffObserver = void (*)(void*);

    bool lowBatteryWarningShownForTest() const { return lowBatteryWarningShown_; }
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

    BatteryManager* battery_ = nullptr;
    V1Display* display_ = nullptr;
    SettingsManager* settings_ = nullptr;
    ShutdownPreparationCallback shutdownPreparationCallback_ = nullptr;
    void* shutdownPreparationContext_ = nullptr;
    ShutdownAbortCallback shutdownAbortCallback_ = nullptr;
    void* shutdownAbortContext_ = nullptr;
#ifdef UNIT_TEST
    ShutdownHandoffObserver shutdownHandoffObserverForTest_ = nullptr;
    void* shutdownHandoffObserverContextForTest_ = nullptr;
#endif

    bool lowBatteryWarningShown_ = false;
    unsigned long criticalBatteryTime_ = 0;

    unsigned long autoPowerOffTimerStart_ = 0; // 0 = timer not running
    bool autoPowerOffArmed_ = false;
    bool v1SignalPresent_ = false;
    bool alpSignalPresent_ = false;
};
