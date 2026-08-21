#include "parsed_frame_event_module.h"

#include "system_event_bus.h"

ParsedFrameSignal ParsedFrameEventModule::collect(bool queueParsedReady, uint32_t queueParsedTsMs,
                                                  SystemEventBus& eventBus) {
    ParsedFrameSignal signal;
    signal.parsedReady = queueParsedReady;
    // The queue retains its last V1 timestamp after the edge is consumed. Only
    // attach that timestamp when this collection actually owns the queue edge.
    signal.parsedTsMs = queueParsedReady ? queueParsedTsMs : 0;

    if (eventBus.consumeAlpStateChanged()) {
        signal.parsedReady = true;
        // ALP owns a different UART clock edge. It may request a display update,
        // but it must not manufacture a V1 notification latency sample.
    }

    return signal;
}
