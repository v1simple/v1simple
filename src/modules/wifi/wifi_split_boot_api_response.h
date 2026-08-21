#pragma once

#include <WebServer.h>

#include "wifi_api_response.h"
#include "wifi_json_document.h"

namespace WifiSplitBootApiResponse {

enum class Operation {
    V1_PUSH_PULL,
    AUTO_PUSH_NOW,
    OBD_RUNTIME,
    ALP_STATUS,
    GPS_STATUS,
};

inline void sendUnavailable(WebServer& server, Operation operation) {
    if (operation == Operation::AUTO_PUSH_NOW) {
        server.send(409, "application/json",
                    "{\"success\":false,\"error\":\"live_push_unavailable_in_maintenance\","
                    "\"message\":\"Live V1 push is unavailable in maintenance mode\"}");
        return;
    }

    WifiJson::Document doc;
    doc["error"] = "maintenance_mode";
    switch (operation) {
    case Operation::V1_PUSH_PULL:
        doc["message"] = "V1 push/pull not available in maintenance mode";
        break;
    case Operation::OBD_RUNTIME:
        doc["message"] = "OBD runtime endpoints are not available in maintenance mode";
        break;
    case Operation::ALP_STATUS:
        doc["message"] = "ALP runtime status is not available in maintenance mode";
        break;
    case Operation::GPS_STATUS:
        doc["message"] = "GPS runtime status is not available in maintenance mode";
        break;
    case Operation::AUTO_PUSH_NOW:
        break;
    }
    WifiApiResponse::sendJsonDocument(server, 409, doc);
}

} // namespace WifiSplitBootApiResponse
