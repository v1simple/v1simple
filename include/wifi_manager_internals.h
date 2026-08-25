/**
 * WiFi Manager internals — shared across wifi_manager TU split.
 *
 * Provides: extern declarations and promoted helper declarations.
 * Each .cpp includes this plus its own module-specific headers.
 */

#pragma once

#include "wifi_manager.h"

// --- Promoted helper declarations ---

/// Map WifiClientState enum to API-facing string name.
inline const char* wifiClientStateApiName(WifiClientState state) {
    switch (state) {
    case WIFI_CLIENT_DISABLED:
        return "disabled";
    case WIFI_CLIENT_DISCONNECTED:
        return "disconnected";
    case WIFI_CLIENT_CONNECTING:
        return "connecting";
    case WIFI_CLIENT_CONNECTED:
        return "connected";
    case WIFI_CLIENT_FAILED:
        return "failed";
    }
    return "unknown";
}

/// Serve a file from LittleFS with ETag/gzip support.
bool serveLittleFSFileHelper(WebServer& server, const char* path, const char* contentType);
