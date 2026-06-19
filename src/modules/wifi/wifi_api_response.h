#pragma once

#include <ArduinoJson.h>
#include <WebServer.h>
#include "json_stream_response.h"

namespace WifiApiResponse {

inline void sendJsonDocument(WebServer& server, int statusCode, const JsonDocument& doc) {
    sendJsonStream(server, doc, statusCode);
}

inline void setErrorAndMessage(JsonDocument& doc, const char* text) {
    const char* value = text ? text : "Unknown error";
    doc["error"] = value;
    doc["message"] = value;
}

}  // namespace WifiApiResponse

