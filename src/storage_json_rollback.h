#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>

enum class JsonRollbackLoadResult {
    Missing = 0,
    LoadedLive,
    LoadedRollback,
    Invalid,
};

JsonRollbackLoadResult loadJsonDocumentWithRollback(fs::FS& fs,
                                                    const char* livePath,
                                                    size_t maxBytes,
                                                    JsonDocument& outDoc,
                                                    String* errorMessage = nullptr,
                                                    String* loadedPath = nullptr);
