#pragma once

#include <cstdint>
#include "esp_err.h"

// Minimal stub for esp_core_dump.h used in native tests.

struct esp_core_dump_summary_t {
    char  exc_task[16];
    struct {
        uint32_t exc_cause;
    } ex_info;
    uint32_t exc_pc;
    struct {
        int      depth;
        uint32_t bt[16];
    } exc_bt_info;
};

inline esp_err_t esp_core_dump_get_summary(esp_core_dump_summary_t* /*summary*/) {
    return -1;  // No coredump available in native tests
}
