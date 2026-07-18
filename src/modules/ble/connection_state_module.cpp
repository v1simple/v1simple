#include "connection_state_module.h"

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
    disconnectPresentationPending_ = false;
    observedSessionGeneration_ = 0;
    openedSessionGeneration_ = 0;
    lastDataRequestMs_ = 0;
}

void ConnectionStateModule::handleSessionOpened(uint32_t sessionGeneration) {
    if (bleQueue_) {
        bleQueue_->openSession(sessionGeneration);
    }
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
        disconnectPresentationPending_ = true;
        if (bus_) {
            SystemEvent event;
            event.type = SystemEventType::BLE_DISCONNECTED;
            event.tsMs = static_cast<uint32_t>(nowMs);
            bus_->publish(event);
        }
    }

    wasConnected_ = false;
    observedSessionGeneration_ = sessionGeneration;
    openedSessionGeneration_ = 0;
}

void ConnectionStateModule::presentDisconnected(unsigned long nowMs) {
    display_->setBleContext(DisplayBleContext{});
    display_->setBLEProxyStatus(false, false, false);
    display_->resetChangeTracking();
    display_->showScanning();
    Serial.printf("[BLE] V1 disconnected; cleared LCD BLE state at %lu ms\n", nowMs);
    CONN_LOG("[BLE] V1 disconnected - scanning");
    disconnectPresentationPending_ = false;
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
    disconnectPresentationPending_ = false;
    if (power_) {
        power_->onV1ConnectionChange(true);
    }
    display_->showResting();
    CONN_LOG("[BLE] V1 connected");
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

    if (!isConnected && disconnectPresentationPending_) {
        presentDisconnected(nowMs);
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
