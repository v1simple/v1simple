#pragma once

#include <WebServer.h>

class AlpRuntimeModule;

// ── ALP HTTP API ─────────────────────────────────────────────────────
//
// Exposes the ALP runtime module's snapshot() state over HTTP so the
// WiFi UI (and external diagnostics) can observe what the ALP serial
// listener is seeing in real time.
//
// Endpoints:
//   GET /api/alp/status  →  JSON snapshot of current ALP state
//
// Threading note: the handler calls alpRuntimeModule.snapshot() which
// reads module fields without synchronization. This is safe because the
// ESP32 Arduino WebServer dispatches handlers synchronously from the
// main loop on Core 1 — the same task that runs alpRuntimeModule.process().
// If this is ever migrated to AsyncWebServer or pinned to Core 0, the
// snapshot() path will need to be hardened (see alp_runtime_module.h).

namespace AlpApiService {

void handleApiStatus(WebServer& server,
                     AlpRuntimeModule& alpRuntime,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx,
                     bool maintenanceBootActive = false);

}  // namespace AlpApiService
