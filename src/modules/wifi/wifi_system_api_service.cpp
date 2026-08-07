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
    if (!runtime.persistSettings || !runtime.markCleanShutdown || !runtime.restart) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"reboot_runtime_unavailable\"}");
        return;
    }

    // Preserve the same ordering used by the physical maintenance-exit path:
    // settings first, then the clean-shutdown marker, then restart. Send the
    // response before the short drain window so the UI can transition cleanly.
    runtime.persistSettings(runtime.ctx);
    runtime.markCleanShutdown(runtime.ctx);
    server.send(202, "application/json", "{\"success\":true,\"rebooting\":true,\"target\":\"normal\"}");
    if (runtime.delayBeforeRestart) {
        runtime.delayBeforeRestart(100, runtime.ctx);
    }
    runtime.restart(runtime.ctx);
}

} // namespace WifiSystemApiService
