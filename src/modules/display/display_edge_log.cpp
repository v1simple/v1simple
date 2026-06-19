/**
 * Display Edge Logging Implementation
 */

#include "display_edge_log.h"

#include <cstdarg>
#include <cstdio>

#include "modules/alp/alp_runtime_module.h"

#ifndef UNIT_TEST

namespace {

void logEdgeLine(uint32_t nowMs, const char* event, const char* fmt, ...) {
    char detail[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(detail, sizeof(detail), fmt, args);
    va_end(args);

    alpLogDisplayDecision(nowMs, event, detail);
}

}  // namespace

void logV1DisplaySetterEdge(uint32_t nowMs, const char* setter, const char* detail) {
    if (detail && detail[0]) {
        logEdgeLine(nowMs, "DISP_V1_SETTER",
                    "%s detail=%s",
                    setter, detail);
    } else {
        logEdgeLine(nowMs, "DISP_V1_SETTER", "%s", setter);
    }
}

#endif
