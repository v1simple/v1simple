#include "connection_cycle_coordinator_module.h"

#include "../../settings_sanitize.h"

namespace {

// If no V1 connects within this window, advance past SCAN_V1 so WiFi
// (and other downstream phases) can proceed without a V1 present.
constexpr uint32_t kScanV1FallbackMs = 30000;
// Auto-push begins only after the V1 connection burst settles (up to 2.5 s),
// so its terminal deadline must remain distinct from the shorter configurable
// quiet fallback used when auto-push is not blocking the phase.
constexpr uint32_t kV1SettleHardDeadlineMs = 10000;

bool hasElapsed(const uint32_t nowMs, const uint32_t startMs, const uint32_t durationMs) {
    return static_cast<int32_t>(nowMs - startMs) >= static_cast<int32_t>(durationMs);
}

bool isObdConnectingState(const ObdConnectionState state) {
    switch (state) {
    case ObdConnectionState::CONNECTING:
    case ObdConnectionState::SECURING:
    case ObdConnectionState::DISCOVERING:
    case ObdConnectionState::AT_INIT:
        return true;
    default:
        return false;
    }
}

bool isObdConnectFailureState(const ObdConnectionState state) {
    switch (state) {
    case ObdConnectionState::IDLE:
    case ObdConnectionState::WAIT_BOOT:
    case ObdConnectionState::DISCONNECTED:
    case ObdConnectionState::ERROR_BACKOFF:
    case ObdConnectionState::ECU_IDLE:
        return true;
    default:
        return false;
    }
}

bool shouldOpenPassiveProxyWindow(const CycleContext& ctx) {
    if (!ctx.proxyEnabled) {
        return false;
    }

    // When OBD has already settled, opening the proxy advertising window
    // would invite a phone/proxy connection into a three-way BLE load
    // (V1 + OBD + phone). OBD speed ingest is higher priority than the
    // maintenance proxy, so the proxy waits for a later non-OBD cycle or
    // explicit debug control.
    if (ctx.obdConnected) {
        return false;
    }

    if (!ctx.obdEnabled || !ctx.obdSavedAddressValid) {
        return true;
    }

    return ctx.proxyClientConnected || ctx.proxyClientConnectedOnceThisBoot;
}

bool isExplicitProxyAppMode(const CycleContext& ctx) {
    return ctx.proxyEnabled && !ctx.obdEnabled;
}

CycleState nextPostObdState(const CycleContext& ctx) {
    if (shouldOpenPassiveProxyWindow(ctx)) {
        return CycleState::PROXY_OPEN;
    }

    return CycleState::STEADY;
}

} // namespace

void ConnectionCycleCoordinatorModule::begin(const Providers& hooks) {
    providers = hooks;
    reset();
}

void ConnectionCycleCoordinatorModule::reset() {
    state_ = CycleState::SCAN_V1;
    stateEnteredMs_ = 0;
    stateEnteredMsValid_ = false;
    totalTransitionCount_ = 0;
    lastObdAttemptMs_ = 0;
    teardownStepStartedMs_ = 0;
    teardownStep_ = TeardownStep::Idle;
    wasV1Connected_ = false;
    lastProxyClientConnected_ = false;
    lastV1Connected_ = false;
    v1VerifyPushMatched_ = false;
    v1VerifyPushMatchedAtMs_ = 0;
    manualWifiPreemptRequested_ = false;
    lastTeardownDurationMs_ = 0;
    totalObdRetryAttempts_ = 0;
    totalWifiManualPhoneKicks_ = 0;
    obdScanWindowMs_ = kConnectionCycleObdScanWindowMsDefault;
    obdRetryIntervalMs_ = kConnectionCycleObdRetryIntervalMsDefault;
    proxyOpenWindowMs_ = kConnectionCycleProxyOpenWindowMsDefault;
    wifiOpenTimeoutMs_ = kConnectionCycleWifiOpenTimeoutMsDefault;
    v1SettleQuietMs_ = kConnectionCycleV1SettleQuietMsDefault;
    v1SettleFallbackMs_ = kConnectionCycleV1SettleFallbackMsDefault;
    teardownAckTimeoutMs_ = kConnectionCycleTeardownAckTimeoutMsDefault;
}

void ConnectionCycleCoordinatorModule::update(const CycleContext& ctx) {
    manualWifiPreemptRequested_ = false;
    if (!stateEnteredMsValid_) {
        stateEnteredMs_ = ctx.nowMs;
        stateEnteredMsValid_ = true;
    }

    updateTimingConfig(ctx);

    if (ctx.v1VerifyPushMatchEdge) {
        v1VerifyPushMatched_ = true;
        v1VerifyPushMatchedAtMs_ = ctx.nowMs;
    }

    const bool v1Connected = !wasV1Connected_ && ctx.v1GattConnected;
    const bool v1Dropped = wasV1Connected_ && !ctx.v1GattConnected;
    if (v1Dropped && state_ != CycleState::TEARDOWN && state_ != CycleState::SCAN_V1) {
        enterTeardown(ctx.nowMs);
        wasV1Connected_ = ctx.v1GattConnected;
        lastProxyClientConnected_ = ctx.proxyClientConnected;
        lastV1Connected_ = ctx.v1GattConnected;
        return;
    }

    const uint32_t v1QuietAnchorMs =
        (v1VerifyPushMatchedAtMs_ > ctx.v1LastEventMs) ? v1VerifyPushMatchedAtMs_ : ctx.v1LastEventMs;
    // Explicit Proxy / App mode must open its phone-advertising window in
    // drive mode even if auto-push is globally enabled; there may be no profile
    // write/readback edge to wait for, and OBD is intentionally disabled.
    const bool autoPushBlocksV1Settle = ctx.autoPushEnabled && !isExplicitProxyAppMode(ctx);
    const bool v1SettledByVerifyPush =
        ctx.v1GattConnected && v1VerifyPushMatched_ && hasElapsed(ctx.nowMs, v1QuietAnchorMs, v1SettleQuietMs_);
    const bool v1SettledByFallback =
        ctx.v1GattConnected && !autoPushBlocksV1Settle && hasElapsed(ctx.nowMs, ctx.v1LastEventMs, v1SettleFallbackMs_);
    // Auto-push can complete the phase early through VerifyPush, but a missing
    // verification edge or continuing V1 traffic must never make V1_SETTLING
    // absorbing.
    const bool v1SettledByDeadline =
        ctx.v1GattConnected && hasElapsed(ctx.nowMs, stateEnteredMs_, kV1SettleHardDeadlineMs);
    const bool v1Settled = v1SettledByVerifyPush || v1SettledByFallback || v1SettledByDeadline;
    const bool obdSettled =
        ctx.obdConnected && ctx.obdState == ObdConnectionState::POLLING && ctx.obdHasValidSpeedSample;

    switch (state_) {
    case CycleState::SCAN_V1:
        if (ctx.v1GattConnected) {
            transitionTo(CycleState::V1_SETTLING, ctx.nowMs);
        } else if (ctx.wifiManualStartIntentLatched) {
            transitionTo(CycleState::WIFI_OPEN, ctx.nowMs);
        } else if (ctx.bootReady && hasElapsed(ctx.nowMs, stateEnteredMs_, kScanV1FallbackMs)) {
            transitionTo(CycleState::STEADY, ctx.nowMs);
        }
        break;

    case CycleState::V1_SETTLING:
        if (v1Settled) {
            transitionTo(ctx.obdEnabled ? CycleState::OBD_SCAN : nextPostObdState(ctx), ctx.nowMs);
        }
        break;

    case CycleState::OBD_SCAN:
        if (obdSettled) {
            transitionTo(CycleState::OBD_SETTLED, ctx.nowMs);
        } else if (isObdConnectingState(ctx.obdState)) {
            transitionTo(CycleState::OBD_CONNECT, ctx.nowMs);
        } else if (hasElapsed(ctx.nowMs, stateEnteredMs_, obdScanWindowMs_)) {
            if (providers.stopObdScan) {
                providers.stopObdScan(providers.stopObdScanContext);
            }
            transitionTo(nextPostObdState(ctx), ctx.nowMs);
        }
        break;

    case CycleState::OBD_CONNECT:
        if (obdSettled) {
            transitionTo(CycleState::OBD_SETTLED, ctx.nowMs);
        } else if (isObdConnectFailureState(ctx.obdState)) {
            if (providers.cancelObdConnect) {
                providers.cancelObdConnect(providers.cancelObdConnectContext);
            }
            transitionTo(nextPostObdState(ctx), ctx.nowMs);
        }
        break;

    case CycleState::OBD_SETTLED:
        transitionTo(nextPostObdState(ctx), ctx.nowMs);
        break;

    case CycleState::PROXY_OPEN:
        if (ctx.wifiManualStartIntentLatched) {
            manualWifiPreemptRequested_ = ctx.proxyClientConnected;
            if (ctx.proxyClientConnected) {
                totalWifiManualPhoneKicks_++;
            }
            if (providers.stopProxyAdvertising) {
                providers.stopProxyAdvertising(providers.stopProxyAdvertisingContext);
            }
            if (ctx.proxyClientConnected && providers.disconnectProxyPhone) {
                providers.disconnectProxyPhone(providers.disconnectProxyPhoneContext);
            }
            transitionTo(CycleState::WIFI_OPEN, ctx.nowMs);
        } else if (!isExplicitProxyAppMode(ctx) && !ctx.proxyClientConnected &&
                   hasElapsed(ctx.nowMs, stateEnteredMs_, proxyOpenWindowMs_)) {
            if (providers.stopProxyAdvertising) {
                providers.stopProxyAdvertising(providers.stopProxyAdvertisingContext);
            }
            transitionTo(CycleState::WIFI_OPEN, ctx.nowMs);
        }
        break;

    case CycleState::WIFI_OPEN:
        if (v1Connected) {
            transitionTo(CycleState::V1_SETTLING, ctx.nowMs);
        } else if (ctx.wifiActive || hasElapsed(ctx.nowMs, stateEnteredMs_, wifiOpenTimeoutMs_)) {
            transitionTo(CycleState::STEADY, ctx.nowMs);
        }
        break;

    case CycleState::STEADY:
        if (v1Connected) {
            transitionTo(CycleState::V1_SETTLING, ctx.nowMs);
        } else if (ctx.wifiManualStartIntentLatched && ctx.proxyClientConnected) {
            manualWifiPreemptRequested_ = true;
            totalWifiManualPhoneKicks_++;
            if (providers.disconnectProxyPhone) {
                providers.disconnectProxyPhone(providers.disconnectProxyPhoneContext);
            }
            transitionTo(CycleState::WIFI_OPEN, ctx.nowMs);
        }
        break;

    case CycleState::TEARDOWN:
        updateTeardown(ctx.nowMs);
        break;
    }

    wasV1Connected_ = ctx.v1GattConnected;
    lastProxyClientConnected_ = ctx.proxyClientConnected;
    lastV1Connected_ = ctx.v1GattConnected;
}

bool ConnectionCycleCoordinatorModule::obdScanAllowed() const {
    return state_ == CycleState::OBD_SCAN;
}

bool ConnectionCycleCoordinatorModule::obdConnectAllowed() const {
    return state_ == CycleState::OBD_SCAN || state_ == CycleState::OBD_CONNECT;
}

bool ConnectionCycleCoordinatorModule::obdRetryAllowed(const uint32_t nowMs) const {
    // Auto-retry only makes sense after this cycle has actually attempted OBD.
    // A mid-session OBD re-enable restores WAIT_BOOT/IDLE in the OBD runtime,
    // but does not seed retry cadence until the next coordinator-owned attempt.
    return (state_ == CycleState::PROXY_OPEN || state_ == CycleState::STEADY) && lastV1Connected_ &&
           !lastProxyClientConnected_ && lastObdAttemptMs_ != 0 &&
           hasElapsed(nowMs, lastObdAttemptMs_, obdRetryIntervalMs_);
}

bool ConnectionCycleCoordinatorModule::proxyAdvertisingAllowed() const {
    return state_ == CycleState::PROXY_OPEN;
}

bool ConnectionCycleCoordinatorModule::proxyKeepConnectionAllowed() const {
    // Kept separate from proxyAdvertisingAllowed() so policy can let an
    // attached phone outlive passive advertising. Explicit Proxy / App mode
    // keeps PROXY_OPEN for the drive; passive proxy still times out.
    return state_ == CycleState::PROXY_OPEN;
}

bool ConnectionCycleCoordinatorModule::wifiAutoStartAllowed() const {
    if (state_ != CycleState::WIFI_OPEN && state_ != CycleState::STEADY) {
        return false;
    }
    return !providers.isProxyFullyStopped || providers.isProxyFullyStopped(providers.isProxyFullyStoppedContext);
}

bool ConnectionCycleCoordinatorModule::shouldPreemptProxyForManualWifiStart() const {
    return manualWifiPreemptRequested_;
}

ObdBleArbitrationRequest ConnectionCycleCoordinatorModule::arbitrationRequest() const {
    switch (state_) {
    case CycleState::V1_SETTLING:
    case CycleState::OBD_SCAN:
    case CycleState::OBD_CONNECT:
        return ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD;
    default:
        return ObdBleArbitrationRequest::NONE;
    }
}

uint32_t ConnectionCycleCoordinatorModule::timeInStateMs(const uint32_t nowMs) const {
    if (!stateEnteredMsValid_) {
        return 0;
    }
    return nowMs - stateEnteredMs_;
}

void ConnectionCycleCoordinatorModule::recordObdRetryAttempt(const uint32_t nowMs) {
    lastObdAttemptMs_ = nowMs;
    totalObdRetryAttempts_++;
}

void ConnectionCycleCoordinatorModule::updateTimingConfig(const CycleContext& ctx) {
    obdScanWindowMs_ = clampConnectionCycleObdScanWindowMsValue(
        ctx.obdScanWindowMs == 0 ? kConnectionCycleObdScanWindowMsDefault : static_cast<int64_t>(ctx.obdScanWindowMs));
    obdRetryIntervalMs_ = clampConnectionCycleObdRetryIntervalMsValue(
        ctx.obdRetryIntervalMs == 0 ? kConnectionCycleObdRetryIntervalMsDefault
                                    : static_cast<int64_t>(ctx.obdRetryIntervalMs));
    proxyOpenWindowMs_ = clampConnectionCycleProxyOpenWindowMsValue(ctx.proxyOpenWindowMs == 0
                                                                        ? kConnectionCycleProxyOpenWindowMsDefault
                                                                        : static_cast<int64_t>(ctx.proxyOpenWindowMs));
    wifiOpenTimeoutMs_ = clampConnectionCycleWifiOpenTimeoutMsValue(ctx.wifiOpenTimeoutMs == 0
                                                                        ? kConnectionCycleWifiOpenTimeoutMsDefault
                                                                        : static_cast<int64_t>(ctx.wifiOpenTimeoutMs));
    v1SettleQuietMs_ = clampConnectionCycleV1SettleQuietMsValue(
        ctx.v1SettleQuietMs == 0 ? kConnectionCycleV1SettleQuietMsDefault : static_cast<int64_t>(ctx.v1SettleQuietMs));
    v1SettleFallbackMs_ = clampConnectionCycleV1SettleFallbackMsValue(
        ctx.v1SettleFallbackMs == 0 ? kConnectionCycleV1SettleFallbackMsDefault
                                    : static_cast<int64_t>(ctx.v1SettleFallbackMs));
    teardownAckTimeoutMs_ = clampConnectionCycleTeardownAckTimeoutMsValue(
        ctx.cycleTeardownAckTimeoutMs == 0 ? kConnectionCycleTeardownAckTimeoutMsDefault
                                           : static_cast<int64_t>(ctx.cycleTeardownAckTimeoutMs));
}

void ConnectionCycleCoordinatorModule::transitionTo(const CycleState newState, const uint32_t nowMs) {
    if (state_ == newState) {
        return;
    }

    if (state_ == CycleState::TEARDOWN && newState != CycleState::TEARDOWN && stateEnteredMsValid_) {
        lastTeardownDurationMs_ = nowMs - stateEnteredMs_;
    }

    state_ = newState;
    stateEnteredMs_ = nowMs;
    stateEnteredMsValid_ = true;
    totalTransitionCount_++;

    switch (newState) {
    case CycleState::V1_SETTLING:
        v1VerifyPushMatched_ = false;
        v1VerifyPushMatchedAtMs_ = 0;
        break;

    case CycleState::OBD_SCAN:
    case CycleState::OBD_CONNECT:
        lastObdAttemptMs_ = nowMs;
        break;

    case CycleState::TEARDOWN:
        teardownStep_ = TeardownStep::WaitObdStop;
        teardownStepStartedMs_ = nowMs;
        break;

    default:
        if (newState != CycleState::TEARDOWN) {
            teardownStep_ = TeardownStep::Idle;
            teardownStepStartedMs_ = 0;
        }
        break;
    }
}

void ConnectionCycleCoordinatorModule::enterTeardown(const uint32_t nowMs) {
    v1VerifyPushMatched_ = false;
    v1VerifyPushMatchedAtMs_ = 0;
    manualWifiPreemptRequested_ = false;
    transitionTo(CycleState::TEARDOWN, nowMs);
    if (providers.stopObdScan) {
        providers.stopObdScan(providers.stopObdScanContext);
    }
    if (providers.cancelObdConnect) {
        providers.cancelObdConnect(providers.cancelObdConnectContext);
    }
}

void ConnectionCycleCoordinatorModule::updateTeardown(const uint32_t nowMs) {
    switch (teardownStep_) {
    case TeardownStep::Idle:
        transitionTo(CycleState::SCAN_V1, nowMs);
        return;

    case TeardownStep::WaitObdStop: {
        const bool obdScanStopped =
            !providers.isObdScanStopped || providers.isObdScanStopped(providers.isObdScanStoppedContext);
        const bool obdConnectIdle =
            !providers.isObdConnectIdle || providers.isObdConnectIdle(providers.isObdConnectIdleContext);
        if ((obdScanStopped && obdConnectIdle) || hasElapsed(nowMs, teardownStepStartedMs_, teardownAckTimeoutMs_)) {
            if (providers.stopProxyAdvertising) {
                providers.stopProxyAdvertising(providers.stopProxyAdvertisingContext);
            }
            if (providers.disconnectProxyPhone) {
                providers.disconnectProxyPhone(providers.disconnectProxyPhoneContext);
            }
            teardownStep_ = TeardownStep::WaitProxyStop;
            teardownStepStartedMs_ = nowMs;
        }
        return;
    }

    case TeardownStep::WaitProxyStop: {
        const bool proxyStopped =
            !providers.isProxyFullyStopped || providers.isProxyFullyStopped(providers.isProxyFullyStoppedContext);
        if (proxyStopped || hasElapsed(nowMs, teardownStepStartedMs_, teardownAckTimeoutMs_)) {
            teardownStep_ = TeardownStep::Idle;
            transitionTo(CycleState::SCAN_V1, nowMs);
        }
        return;
    }
    }
}
