#pragma once

#include <stdint.h>

#include "connection_state_cadence_module.h"

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
    struct Providers {
        ConnectionStateCadenceDecision (*runCadence)(
            void* ctx, const ConnectionStateCadenceContext& cadenceCtx) = nullptr;
        void* cadenceContext = nullptr;

        void (*runConnectionStateProcess)(void* ctx, uint32_t nowMs) = nullptr;
        void* connectionStateContext = nullptr;

        void (*recordDecision)(void* ctx, const ConnectionStateDispatchDecision& decision) = nullptr;
        void* decisionContext = nullptr;
    };

    void begin(const Providers& hooks);
    void reset();
    ConnectionStateDispatchDecision process(const ConnectionStateDispatchContext& ctx);

private:
    Providers providers{};
    uint32_t lastProcessRunMs_ = 0;
    bool hasRunProcess_ = false;
};
