#pragma once

#include <utility>

#include "json_exact_input.h"
#include "psram_json_document.h"

namespace UsbProfileJson {

using Document = PsramJson::Document;

enum class ParseStatus : uint8_t {
    Ok = 0,
    Invalid,
    MemoryUnavailable,
};

// Own the PSRAM-backed document and invoke the consumer only after a complete
// bounded parse. This keeps allocation failure on the no-mutation side of the
// USB restore boundary and makes that ordering directly testable.
template <typename Consumer>
bool parseAndConsume(const uint8_t* data, size_t length, ParseStatus& status, Consumer&& consumer) {
    const ExactJsonInput::Status exact = ExactJsonInput::validate(data, length);
    if (exact == ExactJsonInput::Status::MemoryUnavailable) {
        status = ParseStatus::MemoryUnavailable;
        return false;
    }
    if (exact != ExactJsonInput::Status::Ok) {
        status = ParseStatus::Invalid;
        return false;
    }
    Document document;
    const auto parsed = deserializeJson(document, data, length, DeserializationOption::NestingLimit(8));
    if (document.overflowed() || parsed == DeserializationError::NoMemory) {
        status = ParseStatus::MemoryUnavailable;
        return false;
    }
    if (parsed) {
        status = ParseStatus::Invalid;
        return false;
    }
    status = ParseStatus::Ok;
    return std::forward<Consumer>(consumer)(document);
}

} // namespace UsbProfileJson
