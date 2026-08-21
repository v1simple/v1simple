#include "parsed_frame_event_module.h"

#include "system_event_bus.h"

ParsedFrameSignal ParsedFrameEventModule::collect(bool queueParsedReady, uint32_t queueParsedTsMs,
                                                  SystemEventBus& eventBus) {
    ParsedFrameSignal signal;
    signal.parsedReady = queueParsedReady;
    // The queue retains its last V1 timestamp after the edge is consumed. Only
    // attach that timestamp when this collection actually owns the queue edge.
    signal.parsedTsMs = queueParsedReady ? queueParsedTsMs : 0;

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
        // ALP owns a different UART clock edge. It may request a display update,
        // but it must not manufacture a V1 notification latency sample.
    }

    return signal;
}
