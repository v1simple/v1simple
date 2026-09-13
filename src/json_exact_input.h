#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <esp_heap_caps.h>

// ArduinoJson accepts a complete root followed by non-whitespace and replaces
// an earlier object member when the same key appears again. Those behaviors
// make CRC and schema validation observe a different document from the bytes
// the operator supplied. This allocation-bounded lexical pass proves one
// complete JSON value, valid UTF-8 strings, and unique object keys before the
// DOM is trusted. Key bookkeeping lives only in PSRAM and fails closed.
namespace ExactJsonInput {

enum class Status : uint8_t {
    Ok = 0,
    Invalid,
    MemoryUnavailable,
};

// The pinned ArduinoJson formatter writes valid UTF-8 verbatim and only has
// canonical escapes for the five JSON control characters below.  Form/query
// inputs do not pass through Parser, so mutation endpoints use this helper to
// reject bytes that could not be serialized back as valid JSON.
inline bool validSemanticString(const char* data, size_t length) {
    if ((!data && length != 0) || (data && std::strlen(data) != length)) return false;
    size_t position = 0;
    while (position < length) {
        const uint8_t first = static_cast<uint8_t>(data[position++]);
        if (first < 0x20u) {
            if (first != 0x08u && first != 0x09u && first != 0x0au &&
                first != 0x0cu && first != 0x0du) return false;
            continue;
        }
        if (first < 0x80u) continue;
        uint8_t continuationCount = 0;
        uint32_t codePoint = 0;
        uint32_t minimum = 0;
        if ((first & 0xe0u) == 0xc0u) {
            continuationCount = 1;
            codePoint = first & 0x1fu;
            minimum = 0x80u;
        } else if ((first & 0xf0u) == 0xe0u) {
            continuationCount = 2;
            codePoint = first & 0x0fu;
            minimum = 0x800u;
        } else if ((first & 0xf8u) == 0xf0u) {
            continuationCount = 3;
            codePoint = first & 0x07u;
            minimum = 0x10000u;
        } else {
            return false;
        }
        if (position + continuationCount > length) return false;
        for (uint8_t index = 0; index < continuationCount; ++index) {
            const uint8_t byte = static_cast<uint8_t>(data[position++]);
            if ((byte & 0xc0u) != 0x80u) return false;
            codePoint = (codePoint << 6u) | (byte & 0x3fu);
        }
        if (codePoint < minimum || codePoint > 0x10ffffu ||
            (codePoint >= 0xd800u && codePoint <= 0xdfffu)) return false;
    }
    return true;
}

namespace Detail {

struct KeyRecord {
    uint32_t objectId;
    uint32_t codePoints;
    uint64_t hash;
    size_t rawStart;
    size_t rawEnd;
};

class Parser {
  public:
    Parser(const uint8_t* data, size_t length) : data_(data), length_(length) {}
    ~Parser() { heap_caps_free(keys_); }

    Status parse() {
        if (!data_ || length_ == 0) return Status::Invalid;
        skipWhitespace();
        if (!parseValue(0)) return status_;
        skipWhitespace();
        return status_ == Status::Ok && position_ == length_ ? Status::Ok : Status::Invalid;
    }

  private:
    static constexpr uint8_t kMaxDepth = 32;
    static constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
    static constexpr uint64_t kFnvPrime = 1099511628211ULL;

    bool parseValue(uint8_t depth) {
        if (depth > kMaxDepth || position_ >= length_) return invalid();
        switch (data_[position_]) {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"': return parseString(nullptr, nullptr);
            case 't': return consumeLiteral("true");
            case 'f': return consumeLiteral("false");
            case 'n': return consumeLiteral("null");
            default: return parseNumber();
        }
    }

    bool parseObject(uint8_t depth) {
        const uint32_t objectId = ++nextObjectId_;
        ++position_;
        skipWhitespace();
        if (consume('}')) return true;
        while (position_ < length_) {
            uint64_t hash = kFnvOffset;
            uint32_t codePoints = 0;
            size_t rawStart = 0;
            size_t rawEnd = 0;
            if (!parseString(&hash, &codePoints, &rawStart, &rawEnd) ||
                !rememberUniqueKey(objectId, hash, codePoints, rawStart, rawEnd)) return false;
            skipWhitespace();
            if (!consume(':')) return invalid();
            skipWhitespace();
            if (!parseValue(static_cast<uint8_t>(depth + 1u))) return false;
            skipWhitespace();
            if (consume('}')) return true;
            if (!consume(',')) return invalid();
            skipWhitespace();
        }
        return invalid();
    }

    bool parseArray(uint8_t depth) {
        ++position_;
        skipWhitespace();
        if (consume(']')) return true;
        while (position_ < length_) {
            if (!parseValue(static_cast<uint8_t>(depth + 1u))) return false;
            skipWhitespace();
            if (consume(']')) return true;
            if (!consume(',')) return invalid();
            skipWhitespace();
        }
        return invalid();
    }

    bool parseString(uint64_t* hash, uint32_t* codePoints, size_t* rawStart = nullptr,
                     size_t* rawEnd = nullptr) {
        if (!consume('"')) return invalid();
        if (rawStart) *rawStart = position_;
        while (position_ < length_) {
            uint32_t codePoint = 0;
            const uint8_t byte = data_[position_++];
            if (byte == '"') {
                if (rawEnd) *rawEnd = position_ - 1u;
                return true;
            }
            if (byte < 0x20) return invalid();
            if (byte == '\\') {
                if (position_ >= length_) return invalid();
                const uint8_t escaped = data_[position_++];
                switch (escaped) {
                    case '"': codePoint = '"'; break;
                    case '\\': codePoint = '\\'; break;
                    case '/': codePoint = '/'; break;
                    case 'b': codePoint = 0x08; break;
                    case 'f': codePoint = 0x0c; break;
                    case 'n': codePoint = 0x0a; break;
                    case 'r': codePoint = 0x0d; break;
                    case 't': codePoint = 0x09; break;
                    case 'u': {
                        uint32_t first = 0;
                        if (!parseHex4(first)) return false;
                        if (first >= 0xd800 && first <= 0xdbff) {
                            if (position_ + 2u > length_ || data_[position_] != '\\' || data_[position_ + 1u] != 'u')
                                return invalid();
                            position_ += 2u;
                            uint32_t second = 0;
                            if (!parseHex4(second) || second < 0xdc00 || second > 0xdfff) return invalid();
                            codePoint = 0x10000u + ((first - 0xd800u) << 10u) + (second - 0xdc00u);
                        } else if (first >= 0xdc00 && first <= 0xdfff) {
                            return invalid();
                        } else {
                            codePoint = first;
                        }
                        break;
                    }
                    default: return invalid();
                }
            } else if (byte < 0x80) {
                codePoint = byte;
            } else if (!parseUtf8(byte, codePoint)) {
                return false;
            }
            // No setting/profile field has a binary-string contract. Reject
            // decoded NUL and C0 values the pinned ArduinoJson serializer does
            // not escape; accepting them would let a valid ingress become an
            // invalid JSON export. Keep the five JSON-safe escaped controls.
            if (codePoint < 0x20u && codePoint != 0x08u && codePoint != 0x09u &&
                codePoint != 0x0au && codePoint != 0x0cu && codePoint != 0x0du) return invalid();
            if (hash) {
                // Hash decoded Unicode scalar values, not their source spelling,
                // so `"a"` and `"\u0061"` are the same member name.
                for (uint8_t shift = 0; shift < 4; ++shift) {
                    *hash ^= static_cast<uint8_t>((codePoint >> (shift * 8u)) & 0xffu);
                    *hash *= kFnvPrime;
                }
                ++(*codePoints);
            }
        }
        return invalid();
    }

    bool parseUtf8(uint8_t first, uint32_t& codePoint) {
        uint8_t continuationCount = 0;
        uint32_t minimum = 0;
        if ((first & 0xe0u) == 0xc0u) {
            continuationCount = 1;
            codePoint = first & 0x1fu;
            minimum = 0x80;
        } else if ((first & 0xf0u) == 0xe0u) {
            continuationCount = 2;
            codePoint = first & 0x0fu;
            minimum = 0x800;
        } else if ((first & 0xf8u) == 0xf0u) {
            continuationCount = 3;
            codePoint = first & 0x07u;
            minimum = 0x10000;
        } else {
            return invalid();
        }
        if (position_ + continuationCount > length_) return invalid();
        for (uint8_t index = 0; index < continuationCount; ++index) {
            const uint8_t byte = data_[position_++];
            if ((byte & 0xc0u) != 0x80u) return invalid();
            codePoint = (codePoint << 6u) | (byte & 0x3fu);
        }
        return (codePoint >= minimum && codePoint <= 0x10ffffu &&
                !(codePoint >= 0xd800u && codePoint <= 0xdfffu)) || invalid();
    }

    bool parseHex4(uint32_t& value) {
        if (position_ + 4u > length_) return invalid();
        value = 0;
        for (uint8_t index = 0; index < 4; ++index) {
            const uint8_t byte = data_[position_++];
            uint8_t digit = 0;
            if (byte >= '0' && byte <= '9') digit = byte - '0';
            else if (byte >= 'a' && byte <= 'f') digit = static_cast<uint8_t>(byte - 'a' + 10u);
            else if (byte >= 'A' && byte <= 'F') digit = static_cast<uint8_t>(byte - 'A' + 10u);
            else return invalid();
            value = (value << 4u) | digit;
        }
        return true;
    }

    bool parseNumber() {
        const size_t start = position_;
        consume('-');
        if (position_ >= length_) return invalid();
        if (consume('0')) {
            if (position_ < length_ && data_[position_] >= '0' && data_[position_] <= '9') return invalid();
        } else {
            if (data_[position_] < '1' || data_[position_] > '9') return invalid();
            while (position_ < length_ && data_[position_] >= '0' && data_[position_] <= '9') ++position_;
        }
        if (consume('.')) {
            if (position_ >= length_ || data_[position_] < '0' || data_[position_] > '9') return invalid();
            while (position_ < length_ && data_[position_] >= '0' && data_[position_] <= '9') ++position_;
        }
        if (position_ < length_ && (data_[position_] == 'e' || data_[position_] == 'E')) {
            ++position_;
            if (position_ < length_ && (data_[position_] == '+' || data_[position_] == '-')) ++position_;
            if (position_ >= length_ || data_[position_] < '0' || data_[position_] > '9') return invalid();
            while (position_ < length_ && data_[position_] >= '0' && data_[position_] <= '9') ++position_;
        }
        return position_ > start;
    }

    bool consumeLiteral(const char* literal) {
        const size_t count = std::strlen(literal);
        if (position_ + count > length_ || std::memcmp(data_ + position_, literal, count) != 0) return invalid();
        position_ += count;
        return true;
    }

    bool decodedKeysEqual(size_t lhs, size_t lhsEnd, size_t rhs, size_t rhsEnd) const {
        while (lhs < lhsEnd && rhs < rhsEnd) {
            uint32_t lhsCodePoint = 0;
            uint32_t rhsCodePoint = 0;
            if (!decodeKeyCodePoint(lhs, lhsEnd, lhsCodePoint) ||
                !decodeKeyCodePoint(rhs, rhsEnd, rhsCodePoint) || lhsCodePoint != rhsCodePoint) return false;
        }
        return lhs == lhsEnd && rhs == rhsEnd;
    }

    bool decodeKeyCodePoint(size_t& cursor, size_t end, uint32_t& codePoint) const {
        if (cursor >= end) return false;
        const uint8_t first = data_[cursor++];
        if (first == '\\') {
            if (cursor >= end) return false;
            const uint8_t escaped = data_[cursor++];
            switch (escaped) {
                case '"': codePoint = '"'; return true;
                case '\\': codePoint = '\\'; return true;
                case '/': codePoint = '/'; return true;
                case 'b': codePoint = 0x08; return true;
                case 'f': codePoint = 0x0c; return true;
                case 'n': codePoint = 0x0a; return true;
                case 'r': codePoint = 0x0d; return true;
                case 't': codePoint = 0x09; return true;
                case 'u': {
                    uint32_t firstUnit = 0;
                    if (!decodeHex4(cursor, end, firstUnit)) return false;
                    if (firstUnit >= 0xd800u && firstUnit <= 0xdbffu) {
                        if (cursor + 2u > end || data_[cursor] != '\\' || data_[cursor + 1u] != 'u') return false;
                        cursor += 2u;
                        uint32_t secondUnit = 0;
                        if (!decodeHex4(cursor, end, secondUnit) || secondUnit < 0xdc00u || secondUnit > 0xdfffu)
                            return false;
                        codePoint = 0x10000u + ((firstUnit - 0xd800u) << 10u) + (secondUnit - 0xdc00u);
                    } else {
                        codePoint = firstUnit;
                    }
                    return true;
                }
                default: return false;
            }
        }
        if (first < 0x80u) {
            codePoint = first;
            return true;
        }
        uint8_t continuationCount = 0;
        if ((first & 0xe0u) == 0xc0u) {
            continuationCount = 1;
            codePoint = first & 0x1fu;
        } else if ((first & 0xf0u) == 0xe0u) {
            continuationCount = 2;
            codePoint = first & 0x0fu;
        } else if ((first & 0xf8u) == 0xf0u) {
            continuationCount = 3;
            codePoint = first & 0x07u;
        } else {
            return false;
        }
        if (cursor + continuationCount > end) return false;
        for (uint8_t index = 0; index < continuationCount; ++index) {
            codePoint = (codePoint << 6u) | (data_[cursor++] & 0x3fu);
        }
        return true;
    }

    bool decodeHex4(size_t& cursor, size_t end, uint32_t& value) const {
        if (cursor + 4u > end) return false;
        value = 0;
        for (uint8_t index = 0; index < 4; ++index) {
            const uint8_t byte = data_[cursor++];
            uint8_t digit = 0;
            if (byte >= '0' && byte <= '9') digit = byte - '0';
            else if (byte >= 'a' && byte <= 'f') digit = static_cast<uint8_t>(byte - 'a' + 10u);
            else if (byte >= 'A' && byte <= 'F') digit = static_cast<uint8_t>(byte - 'A' + 10u);
            else return false;
            value = (value << 4u) | digit;
        }
        return true;
    }

    bool rememberUniqueKey(uint32_t objectId, uint64_t hash, uint32_t codePoints,
                           size_t rawStart, size_t rawEnd) {
        for (size_t index = 0; index < keyCount_; ++index) {
            if (keys_[index].objectId == objectId && keys_[index].hash == hash &&
                keys_[index].codePoints == codePoints &&
                decodedKeysEqual(keys_[index].rawStart, keys_[index].rawEnd, rawStart, rawEnd)) {
                return invalid();
            }
        }
        if (keyCount_ == keyCapacity_) {
            const size_t nextCapacity = keyCapacity_ == 0 ? 32u : keyCapacity_ * 2u;
            if (nextCapacity < keyCapacity_ || nextCapacity > SIZE_MAX / sizeof(KeyRecord)) return invalid();
            void* resized = heap_caps_realloc(keys_, nextCapacity * sizeof(KeyRecord),
                                              MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
            if (!resized) {
                status_ = Status::MemoryUnavailable;
                return false;
            }
            keys_ = static_cast<KeyRecord*>(resized);
            keyCapacity_ = nextCapacity;
        }
        keys_[keyCount_++] = KeyRecord{objectId, codePoints, hash, rawStart, rawEnd};
        return true;
    }

    void skipWhitespace() {
        while (position_ < length_ &&
               (data_[position_] == ' ' || data_[position_] == '\t' || data_[position_] == '\r' ||
                data_[position_] == '\n')) {
            ++position_;
        }
    }

    bool consume(uint8_t byte) {
        if (position_ < length_ && data_[position_] == byte) {
            ++position_;
            return true;
        }
        return false;
    }

    bool invalid() {
        if (status_ == Status::Ok) status_ = Status::Invalid;
        return false;
    }

    const uint8_t* data_ = nullptr;
    size_t length_ = 0;
    size_t position_ = 0;
    KeyRecord* keys_ = nullptr;
    size_t keyCount_ = 0;
    size_t keyCapacity_ = 0;
    uint32_t nextObjectId_ = 0;
    Status status_ = Status::Ok;
};

} // namespace Detail

inline Status validate(const uint8_t* data, size_t length) {
    Detail::Parser parser(data, length);
    return parser.parse();
}

inline Status validate(const char* data, size_t length) {
    return validate(reinterpret_cast<const uint8_t*>(data), length);
}

} // namespace ExactJsonInput
