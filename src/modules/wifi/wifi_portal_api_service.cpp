#include "wifi_portal_api_service.h"

namespace WifiPortalApiService {

namespace {

void applyCaptivePortalNoStoreHeaders(WebServer& server) {
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
}

} // namespace

void handleApiPing(WebServer& server, void (*markUiActivity)(void*), void* ctx) {
    if (markUiActivity)
        markUiActivity(ctx);
    server.send(200, "text/plain", "OK");
}

void handleApiGenerate204(WebServer& server, void (*markUiActivity)(void*), void* ctx) {
    if (markUiActivity)
        markUiActivity(ctx);
    applyCaptivePortalNoStoreHeaders(server);
    server.send(204, "text/plain", "");
}

void handleApiGen204(WebServer& server, void (*markUiActivity)(void*), void* ctx) {
    if (markUiActivity)
        markUiActivity(ctx);
    applyCaptivePortalNoStoreHeaders(server);
    server.send(204, "text/plain", "");
}

void handleApiHotspotDetect(WebServer& server, void (*markUiActivity)(void*), void* ctx) {
    if (markUiActivity)
        markUiActivity(ctx);
    applyCaptivePortalNoStoreHeaders(server);
    server.sendHeader("Location", "/settings", true);
    server.send(302, "text/html", "");
}

void handleApiFwlink(WebServer& server) {
    applyCaptivePortalNoStoreHeaders(server);
    server.sendHeader("Location", "/settings", true);
    server.send(302, "text/html", "");
}

void handleApiNcsiTxt(WebServer& server) {
    applyCaptivePortalNoStoreHeaders(server);
    server.send(200, "text/plain", "Microsoft NCSI");
}

} // namespace WifiPortalApiService
