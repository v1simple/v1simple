#include "connection_state_module.h"

// Native test builds pre-include mock headers (test/mocks) before pulling this
// translation unit in directly — same pattern as display_orchestration_module.
#ifndef UNIT_TEST
#include "display_layout.h"
#include "ble_client.h"
#include "display.h"
#include "modules/ble/ble_queue_module.h"
#include "modules/power/power_module.h"
#include "modules/system/system_event_bus.h"
#include "packet_parser.h"
#endif

#define CONN_LOG(...)                                                                                                  \
    do {                                                                                                               \
    } while (0)

void ConnectionStateModule::begin(V1BLEClient* bleClient, PacketParser* parserPtr, V1Display* displayPtr,
                                  PowerModule* powerModule, BleQueueModule* bleQueueModule, SystemEventBus* eventBus) {
    ble_ = bleClient;
    parser_ = parserPtr;
    display_ = displayPtr;
    power_ = powerModule;
    bleQueue_ = bleQueueModule;
    bus_ = eventBus;
    wasConnected_ = false;
    lastDataRequestMs_ = 0;
}

bool ConnectionStateModule::process(unsigned long nowMs) {
    if (!ble_ || !parser_ || !display_) {
        return false;
    }

    bool isConnected = ble_->isConnected();

    // Handle state transitions
    if (isConnected != wasConnected_) {
        if (power_) {
            power_->onV1ConnectionChange(isConnected);
        }

        if (isConnected) {
            // Just connected
            display_->showResting();
            CONN_LOG("[BLE] V1 connected");
            if (bus_) {
                SystemEvent event;
                event.type = SystemEventType::BLE_CONNECTED;
                event.tsMs = static_cast<uint32_t>(nowMs);
                bus_->publish(event);
            }
        } else {
            // Just disconnected - reset stale state
            parser_->resetAlertAssembly();
            display_->resetChangeTracking();
            display_->showScanning();
            CONN_LOG("[BLE] V1 disconnected - scanning");
            if (bus_) {
                SystemEvent event;
                event.type = SystemEventType::BLE_DISCONNECTED;
                event.tsMs = static_cast<uint32_t>(nowMs);
                bus_->publish(event);
            }
        }
        wasConnected_ = isConnected;
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
