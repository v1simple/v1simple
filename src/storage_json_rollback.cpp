#include "storage_json_rollback.h"

#include "json_exact_input.h"
#include "psram_json_document.h"
#include "storage_manager.h"

namespace {

enum class JsonPathLoadResult : uint8_t {
    Loaded,
    Invalid,
    OutOfMemory,
};

JsonPathLoadResult loadJsonDocumentAtPath(fs::FS& fs, const char* path, size_t maxBytes,
                                          JsonDocument& outDoc, String* errorMessage) {
    if (errorMessage) {
        *errorMessage = "";
    }
    outDoc.clear();

    if (!path || path[0] == '\0' || !fs.exists(path)) {
        if (errorMessage) {
            *errorMessage = "missing";
        }
        return JsonPathLoadResult::Invalid;
    }

    File file = fs.open(path, FILE_READ);
    if (!file) {
        if (errorMessage) {
            *errorMessage = "open failed";
        }
        return JsonPathLoadResult::Invalid;
    }

    const size_t fileSize = file.size();
    if (fileSize == 0 || fileSize > maxBytes) {
        file.close();
        if (errorMessage) {
            *errorMessage = "size invalid";
        }
        return JsonPathLoadResult::Invalid;
    }

    PsramJson::Buffer bytes(fileSize);
    if (!bytes || file.read(bytes.data(), fileSize) != fileSize) {
        file.close();
        if (errorMessage) *errorMessage = "memory/read unavailable";
        return JsonPathLoadResult::OutOfMemory;
    }
    file.close();
    const ExactJsonInput::Status exact = ExactJsonInput::validate(bytes.data(), fileSize);
    if (exact != ExactJsonInput::Status::Ok) {
        if (errorMessage) *errorMessage = exact == ExactJsonInput::Status::MemoryUnavailable
                                              ? "exact validation memory unavailable"
                                              : "not one exact unique-key UTF-8 document";
        return exact == ExactJsonInput::Status::MemoryUnavailable ? JsonPathLoadResult::OutOfMemory
                                                                  : JsonPathLoadResult::Invalid;
    }
    const DeserializationError err = deserializeJson(outDoc, bytes.data(), fileSize);
    if (err) {
        if (errorMessage) {
            *errorMessage = err.c_str();
        }
        outDoc.clear();
        return err == DeserializationError::NoMemory ? JsonPathLoadResult::OutOfMemory
                                                     : JsonPathLoadResult::Invalid;
    }

    return outDoc.overflowed() ? JsonPathLoadResult::OutOfMemory : JsonPathLoadResult::Loaded;
}

} // namespace

JsonRollbackLoadResult loadJsonDocumentWithRollback(fs::FS& fs, const char* livePath, size_t maxBytes,
                                                    JsonDocument& outDoc, String* errorMessage, String* loadedPath) {
    if (loadedPath) {
        *loadedPath = "";
    }
    if (errorMessage) {
        *errorMessage = "";
    }

    if (!livePath || livePath[0] == '\0') {
        return JsonRollbackLoadResult::Invalid;
    }

    const String rollbackPath = StorageManager::rollbackPathFor(livePath);
    const bool liveExists = fs.exists(livePath);
    const bool rollbackExists = rollbackPath.length() > 0 && fs.exists(rollbackPath.c_str());

    bool allocationFailed = false;
    const JsonPathLoadResult liveResult = liveExists
                                              ? loadJsonDocumentAtPath(fs, livePath, maxBytes, outDoc, errorMessage)
                                              : JsonPathLoadResult::Invalid;
    allocationFailed |= liveResult == JsonPathLoadResult::OutOfMemory;
    if (liveResult == JsonPathLoadResult::Loaded) {
        if (loadedPath) {
            *loadedPath = livePath;
        }
        return JsonRollbackLoadResult::LoadedLive;
    }
    // A transient inability to read the authoritative live candidate is not
    // evidence that an older rollback is preferable. Abort before inspecting
    // or promoting stale state.
    if (liveResult == JsonPathLoadResult::OutOfMemory) {
        outDoc.clear();
        return JsonRollbackLoadResult::OutOfMemory;
    }

    const JsonPathLoadResult rollbackResult = rollbackExists
                                                  ? loadJsonDocumentAtPath(fs, rollbackPath.c_str(), maxBytes,
                                                                           outDoc, errorMessage)
                                                  : JsonPathLoadResult::Invalid;
    allocationFailed |= rollbackResult == JsonPathLoadResult::OutOfMemory;
    if (rollbackResult == JsonPathLoadResult::OutOfMemory) {
        outDoc.clear();
        return JsonRollbackLoadResult::OutOfMemory;
    }
    if (rollbackResult == JsonPathLoadResult::Loaded) {
        if (loadedPath) {
            *loadedPath = rollbackPath;
        }
        return JsonRollbackLoadResult::LoadedRollback;
    }

    if (allocationFailed) {
        return JsonRollbackLoadResult::OutOfMemory;
    }
    return (liveExists || rollbackExists) ? JsonRollbackLoadResult::Invalid : JsonRollbackLoadResult::Missing;
}
