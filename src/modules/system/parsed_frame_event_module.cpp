#include "parsed_frame_event_module.h"

#include "system_event_bus.h"

ParsedFrameSignal ParsedFrameEventModule::collect(bool queueParsedReady, SystemEventBus& eventBus) {
    ParsedFrameSignal signal;
    signal.parsedReady = queueParsedReady;

    if (eventBus.consumeAlpStateChanged()) {
        signal.parsedReady = true;
    }

    return signal;
}
