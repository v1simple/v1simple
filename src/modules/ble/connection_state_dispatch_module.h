#pragma once

#include <stdint.h>

#include "connection_state_cadence_module.h"

class ConnectionStateModule;

struct ConnectionStateDispatchContext {
    uint32_t nowMs = 0;
    uint32_t displayUpdateIntervalMs = 50;
    uint32_t scanScreenDwellMs = 0;
    bool bleConnectedNow = false;
    bool bootSplashHoldActive = false;
    bool displayPreviewRunning = false;
    uint32_t maxProcessGapMs = 0;
};

struct ConnectionStateDispatchDecision {
    ConnectionStateCadenceDecision cadence{};
    uint32_t elapsedSinceLastProcessMs = 0;
    bool watchdogForced = false;
    bool ranConnectionStateProcess = false;
};

// Executes the connection-state cadence gate and applies starvation watchdog safety.
class ConnectionStateDispatchModule {
  public:
    void begin(ConnectionStateCadenceModule& cadence, ConnectionStateModule& connectionState);
    void reset();
    ConnectionStateDispatchDecision process(const ConnectionStateDispatchContext& ctx);

  private:
    ConnectionStateCadenceModule* cadence_ = nullptr;
    ConnectionStateModule* connectionState_ = nullptr;
    uint32_t lastProcessRunMs_ = 0;
    bool hasRunProcess_ = false;
};
