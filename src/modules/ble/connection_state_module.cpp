#include "connection_state_module.h"

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)
#include "modules/ble/ble_bsc05_hil_fault_module.h"
#endif

// Native test builds pre-include mock headers (test/mocks) before pulling this
// translation unit in directly — same pattern as display_orchestration_module.
#ifndef UNIT_TEST
#include "display_layout.h"
#include "ble_client.h"
#include "display.h"
#include "modules/ble/ble_queue_module.h"
#include "modules/alert_persistence/alert_persistence_module.h"
#include "modules/power/power_module.h"
#include "modules/system/system_event_bus.h"
#include "packet_parser.h"
#endif

#define CONN_LOG(...)                                                                                                  \
    do {                                                                                                               \
    } while (0)

void ConnectionStateModule::begin(V1BLEClient* bleClient, PacketParser* parserPtr, V1Display* displayPtr,
                                  PowerModule* powerModule, BleQueueModule* bleQueueModule,
                                  AlertPersistenceModule* alertPersistence, SystemEventBus* eventBus) {
    ble_ = bleClient;
    parser_ = parserPtr;
    display_ = displayPtr;
    power_ = powerModule;
    bleQueue_ = bleQueueModule;
    alertPersistence_ = alertPersistence;
    bus_ = eventBus;
    wasConnected_ = false;
    connectedPresentationPending_ = false;
    disconnectPresentationPending_ = false;
    disconnectDisplayCleanupPending_ = false;
    observedSessionGeneration_ = 0;
    openedSessionGeneration_ = 0;
    lastDataRequestMs_ = 0;
    displayOwnerRestoreCallback_ = nullptr;
    displayOwnerRestoreContext_ = nullptr;
}

void ConnectionStateModule::setDisplayOwnerRestoreCallback(DisplayOwnerRestoreCallback callback, void* context) {
    displayOwnerRestoreCallback_ = callback;
    displayOwnerRestoreContext_ = context;
}

void ConnectionStateModule::handleSessionOpened(uint32_t sessionGeneration) {
    if (bleQueue_) {
        bleQueue_->openSession(sessionGeneration);
    }
#if defined(V1SIMPLE_HIL_FAULT_CONTROL)
    bleBsc05HilFaultModule().recordSessionOpened(sessionGeneration, millis());
#endif
    openedSessionGeneration_ = sessionGeneration;
    observedSessionGeneration_ = sessionGeneration;
}

void ConnectionStateModule::handleSessionClosed(unsigned long nowMs, uint32_t sessionGeneration) {
    if (!parser_ || !display_) {
        return;
    }

    const bool hadUsableConnection = wasConnected_;
    if (bleQueue_) {
        bleQueue_->closeSession();
    }
#if defined(V1SIMPLE_HIL_FAULT_CONTROL)
    bleBsc05HilFaultModule().recordSessionClosed(sessionGeneration, static_cast<uint32_t>(nowMs));
#endif
    parser_->resetAlertState();
    if (alertPersistence_) {
        alertPersistence_->clearPersistence();
    }
    if (bus_) {
        SystemEvent staleFrame;
        while (bus_->consumeByType(SystemEventType::BLE_FRAME_PARSED, staleFrame)) {
        }
    }

    if (hadUsableConnection) {
        if (power_) {
            power_->onV1ConnectionChange(false);
        }
        disconnectDisplayCleanupPending_ = true;
        disconnectPresentationPending_ = true;
        if (bus_) {
            SystemEvent event;
            event.type = SystemEventType::BLE_DISCONNECTED;
            event.tsMs = static_cast<uint32_t>(nowMs);
            bus_->publish(event);
        }
    }

    wasConnected_ = false;
    connectedPresentationPending_ = false;
    observedSessionGeneration_ = sessionGeneration;
    openedSessionGeneration_ = 0;
}

void ConnectionStateModule::presentConnected() {
    display_->showResting();
    CONN_LOG("[BLE] V1 connected");
}

void ConnectionStateModule::presentDisconnected(unsigned long nowMs) {
    display_->showScanning();
    Serial.printf("[BLE] V1 disconnected; cleared LCD BLE state at %lu ms\n", nowMs);
    CONN_LOG("[BLE] V1 disconnected - scanning");
}

void ConnectionStateModule::handleConnected(unsigned long nowMs, uint32_t sessionGeneration) {
    if (!parser_ || !display_) {
        return;
    }
    if (wasConnected_ && observedSessionGeneration_ == sessionGeneration) {
        return;
    }

    if (openedSessionGeneration_ != sessionGeneration) {
        handleSessionOpened(sessionGeneration);
    }
    connectedPresentationPending_ = true;
    disconnectPresentationPending_ = false;
    disconnectDisplayCleanupPending_ = false;
    if (power_) {
        power_->onV1ConnectionChange(true);
    }
    if (bus_) {
        SystemEvent event;
        event.type = SystemEventType::BLE_CONNECTED;
        event.tsMs = static_cast<uint32_t>(nowMs);
        bus_->publish(event);
    }
    wasConnected_ = true;
    observedSessionGeneration_ = sessionGeneration;
}

void ConnectionStateModule::handleDisconnected(unsigned long nowMs) {
    handleSessionClosed(nowMs, ble_ ? ble_->sessionGeneration() : observedSessionGeneration_);
}

void ConnectionStateModule::presentPendingOwner(unsigned long nowMs, bool v1Connected, uint32_t sessionGeneration) {
    bool restored = false;
    if (displayOwnerRestoreCallback_) {
        restored = displayOwnerRestoreCallback_(displayOwnerRestoreContext_, static_cast<uint32_t>(nowMs));
    }

    if (!restored) {
        // A callback may decline after the live edge changes. Do not let the
        // generic fallback briefly paint the now-obsolete captured owner.
        if (!ble_ || ble_->isConnected() != v1Connected || ble_->sessionGeneration() != sessionGeneration) {
            return;
        }
        if (v1Connected) {
            presentConnected();
        } else {
            presentDisconnected(nowMs);
        }
        restored = true;
    }

    if (!restored || !ble_ || ble_->isConnected() != v1Connected || ble_->sessionGeneration() != sessionGeneration) {
        return;
    }

    // Only this owner-aware path may fulfill a queued edge. Ambient parsed,
    // blink and preview-restore renders can run with a BLE display context
    // sampled earlier in the loop, so they deliberately cannot clear it.
    if (v1Connected && wasConnected_ && observedSessionGeneration_ == sessionGeneration) {
        connectedPresentationPending_ = false;
    } else if (!v1Connected && !wasConnected_ && observedSessionGeneration_ == sessionGeneration &&
               !disconnectDisplayCleanupPending_) {
        disconnectPresentationPending_ = false;
    }
}

bool ConnectionStateModule::process(unsigned long nowMs) {
    if (!ble_ || !parser_ || !display_) {
        return false;
    }

    const bool isConnected = ble_->isConnected();
    const uint32_t sessionGeneration = ble_->sessionGeneration();

    // The callback hooks own the normal path. Generation comparison is a
    // watchdog: even if boolean polling misses disconnect+reconnect entirely,
    // a new connected generation forces a full outgoing-session reset.
    if (sessionGeneration != observedSessionGeneration_ && wasConnected_) {
        handleSessionClosed(nowMs, sessionGeneration);
    }
    if (isConnected && (!wasConnected_ || sessionGeneration != observedSessionGeneration_)) {
        handleConnected(nowMs, sessionGeneration);
    } else if (!isConnected && wasConnected_) {
        handleDisconnected(nowMs);
    } else if (!wasConnected_) {
        observedSessionGeneration_ = sessionGeneration;
    }

    // Lifecycle callbacks remain display-free. Apply disconnect-derived
    // context cleanup here immediately before the authoritative owner render.
    if (!isConnected && disconnectDisplayCleanupPending_) {
        display_->setBleContext(DisplayBleContext{});
        display_->setBLEProxyStatus(false, false, false);
        display_->resetChangeTracking();
        disconnectDisplayCleanupPending_ = false;
        disconnectPresentationPending_ = true;
    } else if (isConnected) {
        disconnectDisplayCleanupPending_ = false;
    }

    // process() is dispatched only after the boot-splash, display-preview and
    // scan-dwell cadence gates release. Keep full display work here rather
    // than in handleConnected(), which runs synchronously inside
    // V1BLEClient::process() at subscribe completion.
    if (isConnected && connectedPresentationPending_) {
        presentPendingOwner(nowMs, true, sessionGeneration);
    } else if (!isConnected && disconnectPresentationPending_) {
        presentPendingOwner(nowMs, false, sessionGeneration);
    }

    // If connected but not seeing traffic, periodically re-request alert data
    if (isConnected && bleQueue_) {
        unsigned long lastRx = bleQueue_->getLastRxMillis();
        bool dataStale = (nowMs - lastRx) > DATA_STALE_MS;
        bool canRequest = (nowMs - lastDataRequestMs_) > DATA_REQUEST_INTERVAL_MS;

        if (dataStale && canRequest) {
            ble_->requestAlertData();
            lastDataRequestMs_ = nowMs;
        }
    }

    // When disconnected, refresh indicators in the canvas.
    // No flush here — the display pipeline owns all strip flushing.
    // Left strip is always flushed; battery (right strip) will flush
    // when the pipeline next updates the right strip.
    if (!isConnected) {
        display_->drawWiFiIndicator();
        display_->drawBatteryIndicator();
    }

    return isConnected;
}
