#include "parsed_frame_event_module.h"

#include "system_event_bus.h"

ParsedFrameSignal ParsedFrameEventModule::collect(bool queueParsedReady, uint32_t queueParsedTsMs,
                                                  SystemEventBus& eventBus) {
    ParsedFrameSignal signal;
    signal.parsedReady = queueParsedReady;
    signal.parsedTsMs = queueParsedTsMs;

    // Drain parsed-frame events only; leave other event types for their owners.
    SystemEvent event;
    while (eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event)) {
        signal.parsedReady = true;
        if (event.tsMs != 0) {
            signal.parsedTsMs = event.tsMs;
        }
    }

    while (eventBus.consumeByType(SystemEventType::ALP_STATE_CHANGED, event)) {
        signal.parsedReady = true;
        // Do not overwrite parsedTsMs — V1 timing is still what
        // notify-to-display latency measures against.
    }

    return signal;
}
