// Alert Persistence Module - Implementation
// Handles V1 radar alert display state and persistence

#include "alert_persistence_module.h"

#include "../perf/debug_macros.h"
#include "ble_client.h"
#include "display.h"
#include "packet_parser.h"
#include "perf_metrics.h"
#include "settings.h"

AlertPersistenceModule::AlertPersistenceModule() {
    // Dependencies set in begin()
}

void AlertPersistenceModule::begin(V1BLEClient* ble, PacketParser* pParser, V1Display* disp,
                                   SettingsManager* settings) {
    bleClient_ = ble;
    parser_ = pParser;
    display_ = disp;
    settings_ = settings;

    DBG_PRINTLN("[AlertPersistenceModule] Initialized");
}

// ============================================================================
// Alert Persistence - shows last alert briefly after V1 clears it
// ============================================================================

void AlertPersistenceModule::setPersistedAlert(const AlertData& alert) {
    persistedAlert_ = alert;
    alertPersistenceActive_ = false;
    alertClearedTime_ = 0;
}

void AlertPersistenceModule::startPersistence(unsigned long now) {
    // Every outcome is observable: success increments alertPersistStarts,
    // and each refusal path increments its own counter so the module
    // boundary is not silent on no-op calls.
    if (!persistedAlert_.isValid) {
        // Caller invoked startPersistence() with no valid alert latched.
        // Display pipeline already guards this; any nonzero count here is
        // a call-site regression.
        PERF_INC(alertPersistStartsSkippedInvalid);
        return;
    }
    if (alertClearedTime_ != 0) {
        // Persistence window already in flight. Expected high-frequency
        // no-op — renderIdleOwner() calls us every idle tick during a
        // window and we idempotently ignore repeats.
        PERF_INC(alertPersistStartsSkippedActive);
        return;
    }
    alertClearedTime_ = now;
    alertPersistenceActive_ = true;
    PERF_INC(alertPersistStarts);
}

void AlertPersistenceModule::clearPersistence() {
    const bool hadState = alertPersistenceActive_ || persistedAlert_.isValid || (alertClearedTime_ != 0);
    if (hadState) {
        PERF_INC(alertPersistClears);
    }
    persistedAlert_ = AlertData();
    alertPersistenceActive_ = false;
    alertClearedTime_ = 0;
}

bool AlertPersistenceModule::shouldShowPersisted(unsigned long now, unsigned long persistMs) const {
    return alertPersistenceActive_ && (now - alertClearedTime_) < persistMs;
}
