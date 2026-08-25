#include "wifi_system_api_service.h"

namespace WifiSystemApiService {

void handleApiRebootNormal(WebServer& server, const RebootRuntime& runtime) {
    if (runtime.markUiActivity) {
        runtime.markUiActivity(runtime.ctx);
    }
    if (!runtime.maintenanceBootActive) {
        server.send(409, "application/json", "{\"success\":false,\"error\":\"maintenance_mode_required\"}");
        return;
    }
    if (!runtime.persistSettings || !runtime.prepareCleanRestart || !runtime.restart) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"reboot_runtime_unavailable\"}");
        return;
    }

    // Observability cleanup decides only whether another SD write is safe; it
    // never vetoes the product restart. Send the response before the short
    // drain window so the UI can transition cleanly.
    const bool persistenceSafe = runtime.prepareCleanRestart(runtime.ctx);
    if (persistenceSafe) {
        runtime.persistSettings(runtime.ctx);
    }
    server.send(202, "application/json", "{\"success\":true,\"rebooting\":true,\"target\":\"normal\"}");
    if (runtime.delayBeforeRestart) {
        runtime.delayBeforeRestart(100, runtime.ctx);
    }
    runtime.restart(runtime.ctx);
}

} // namespace WifiSystemApiService
