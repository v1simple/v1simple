#pragma once

#include <WebServer.h>

namespace WifiPortalApiService {

void handleApiPing(WebServer& server, void (*markUiActivity)(void*), void* ctx);

void handleApiGenerate204(WebServer& server, void (*markUiActivity)(void*), void* ctx);

void handleApiGen204(WebServer& server, void (*markUiActivity)(void*), void* ctx);

void handleApiHotspotDetect(WebServer& server, void (*markUiActivity)(void*), void* ctx);

void handleApiFwlink(WebServer& server);

void handleApiNcsiTxt(WebServer& server);

}  // namespace WifiPortalApiService
