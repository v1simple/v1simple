#pragma once

#include <Arduino.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../json_exact_input.h"

// Allocation-free structural validation for bounded form bodies. Entries
// retain spans into the byte-preserving request buffer; individual values are
// decoded only when requested and every String allocation is verified. The
// multipart form is deliberately limited to the string-only shape emitted by
// the shipped maintenance UI: one Content-Disposition header per part, no
// filename/content-type, no preamble, and no epilogue.
class ExactUrlEncodedForm {
  public:
    enum class Status : uint8_t { Valid = 0, Invalid, TooManyFields, MemoryUnavailable };

    ExactUrlEncodedForm(const uint8_t* bytes, size_t size) : bytes_(bytes), size_(size) {
        status_ = parse();
    }

    ExactUrlEncodedForm(const uint8_t* bytes, size_t size,
                        const char* multipartBoundary, size_t multipartBoundaryLength)
        : bytes_(bytes), size_(size), multipartBoundary_(multipartBoundary),
          multipartBoundaryLength_(multipartBoundaryLength), multipart_(true) {
        status_ = parse();
    }

    Status status() const { return status_; }
    bool valid() const { return status_ == Status::Valid; }
    size_t fieldCount() const { return count_; }

    bool has(const char* key) const { return find(key) != nullptr; }

    bool hasOnly(const char* const* allowed, size_t allowedCount) const {
        if (!valid() || (!allowed && allowedCount != 0)) return false;
        for (size_t entry = 0; entry < count_; ++entry) {
            bool known = false;
            for (size_t candidate = 0; candidate < allowedCount; ++candidate) {
                if (decodedEquals(entries_[entry].keyOffset, entries_[entry].keyLength,
                                  allowed[candidate])) {
                    known = true;
                    break;
                }
            }
            if (!known) return false;
        }
        return true;
    }

    // Returns false for an absent key, invalid decoded text, or allocation
    // failure. Call has() first when absence is allowed.
    bool read(const char* key, String& out, bool allowEmpty = false) const {
        const Entry* entry = find(key);
        if (!entry) return false;
        const size_t length = decodedLength(entry->valueOffset, entry->valueLength);
        if (length == kInvalidLength || (!allowEmpty && length == 0)) return false;
        String decoded;
        if (length != 0) decoded.reserve(length);
        size_t cursor = entry->valueOffset;
        const size_t end = cursor + entry->valueLength;
        while (cursor < end) {
            uint8_t value = 0;
            if (!decodeOne(cursor, end, value)) return false;
            const size_t before = decoded.length();
            decoded += static_cast<char>(value);
            if (decoded.length() != before + 1u) return false;
        }
        if (decoded.length() != length ||
            !ExactJsonInput::validSemanticString(decoded.c_str(), decoded.length())) return false;
        out = std::move(decoded);
        return out.length() == length;
    }

  private:
    static constexpr size_t kMaxFields = 32;
    static constexpr size_t kInvalidLength = static_cast<size_t>(-1);

    struct Entry {
        size_t keyOffset = 0;
        size_t keyLength = 0;
        size_t valueOffset = 0;
        size_t valueLength = 0;
    };

    static int hexValue(uint8_t c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool decodeOne(size_t& cursor, size_t end, uint8_t& value) const {
        if (cursor >= end) return false;
        const uint8_t raw = bytes_[cursor++];
        if (!multipart_ && raw == '+') {
            value = ' ';
            return true;
        }
        if (multipart_ || raw != '%') {
            value = raw;
            return true;
        }
        if (end - cursor < 2u) return false;
        const int high = hexValue(bytes_[cursor++]);
        const int low = hexValue(bytes_[cursor++]);
        if (high < 0 || low < 0) return false;
        value = static_cast<uint8_t>((high << 4) | low);
        return true;
    }

    size_t decodedLength(size_t offset, size_t length) const {
        size_t cursor = offset;
        const size_t end = offset + length;
        size_t count = 0;
        while (cursor < end) {
            uint8_t ignored = 0;
            if (!decodeOne(cursor, end, ignored)) return kInvalidLength;
            ++count;
        }
        return count;
    }

    bool decodedSemanticSpanValid(size_t offset, size_t length) const {
        size_t cursor = offset;
        const size_t end = offset + length;
        uint32_t codePoint = 0;
        uint8_t continuation = 0;
        uint32_t minimum = 0;
        while (cursor < end) {
            uint8_t value = 0;
            if (!decodeOne(cursor, end, value)) return false;
            if (continuation != 0) {
                if ((value & 0xC0u) != 0x80u) return false;
                codePoint = (codePoint << 6u) | (value & 0x3Fu);
                if (--continuation == 0 &&
                    (codePoint < minimum || codePoint > 0x10FFFFu ||
                     (codePoint >= 0xD800u && codePoint <= 0xDFFFu))) return false;
                continue;
            }
            if (value < 0x80u) {
                if (value < 0x20u && value != '\b' && value != '\t' && value != '\n' &&
                    value != '\f' && value != '\r') return false;
            } else if ((value & 0xE0u) == 0xC0u) {
                codePoint = value & 0x1Fu;
                continuation = 1;
                minimum = 0x80u;
            } else if ((value & 0xF0u) == 0xE0u) {
                codePoint = value & 0x0Fu;
                continuation = 2;
                minimum = 0x800u;
            } else if ((value & 0xF8u) == 0xF0u) {
                codePoint = value & 0x07u;
                continuation = 3;
                minimum = 0x10000u;
            } else {
                return false;
            }
        }
        return continuation == 0;
    }

    bool decodedEquals(size_t offset, size_t length, const char* expected) const {
        if (!expected) return false;
        size_t cursor = offset;
        const size_t end = offset + length;
        size_t expectedIndex = 0;
        while (cursor < end) {
            uint8_t value = 0;
            if (!decodeOne(cursor, end, value) || expected[expectedIndex] == '\0' ||
                value != static_cast<uint8_t>(expected[expectedIndex])) return false;
            ++expectedIndex;
        }
        return expected[expectedIndex] == '\0';
    }

    bool decodedKeysEqual(const Entry& left, const Entry& right) const {
        size_t leftCursor = left.keyOffset;
        size_t rightCursor = right.keyOffset;
        const size_t leftEnd = left.keyOffset + left.keyLength;
        const size_t rightEnd = right.keyOffset + right.keyLength;
        while (leftCursor < leftEnd && rightCursor < rightEnd) {
            uint8_t leftValue = 0;
            uint8_t rightValue = 0;
            if (!decodeOne(leftCursor, leftEnd, leftValue) ||
                !decodeOne(rightCursor, rightEnd, rightValue) || leftValue != rightValue) return false;
        }
        return leftCursor == leftEnd && rightCursor == rightEnd;
    }

    const Entry* find(const char* key) const {
        if (!valid()) return nullptr;
        for (size_t i = 0; i < count_; ++i) {
            if (decodedEquals(entries_[i].keyOffset, entries_[i].keyLength, key)) return &entries_[i];
        }
        return nullptr;
    }

    bool bytesEqual(size_t offset, const char* expected, size_t expectedLength) const {
        return expected && offset <= size_ && expectedLength <= size_ - offset &&
               std::memcmp(bytes_ + offset, expected, expectedLength) == 0;
    }

    bool bytesEqualIgnoreCase(size_t offset, const char* expected, size_t expectedLength) const {
        if (!expected || offset > size_ || expectedLength > size_ - offset) return false;
        for (size_t i = 0; i < expectedLength; ++i) {
            const uint8_t value = bytes_[offset + i];
            const uint8_t normalized = value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
            const uint8_t reference = static_cast<uint8_t>(expected[i]);
            const uint8_t normalizedReference =
                reference >= 'A' && reference <= 'Z' ? reference + ('a' - 'A') : reference;
            if (normalized != normalizedReference) return false;
        }
        return true;
    }

    const uint8_t* findRaw(size_t offset, const char* needle, size_t needleLength) const {
        if (!needle || needleLength == 0 || offset > size_ || needleLength > size_ - offset) return nullptr;
        for (size_t i = offset; i <= size_ - needleLength; ++i) {
            if (std::memcmp(bytes_ + i, needle, needleLength) == 0) return bytes_ + i;
        }
        return nullptr;
    }

    Status addEntry(const Entry& entry) {
        if (count_ >= entries_.size()) return Status::TooManyFields;
        if (entry.keyLength == 0 ||
            decodedLength(entry.keyOffset, entry.keyLength) == kInvalidLength ||
            decodedLength(entry.valueOffset, entry.valueLength) == kInvalidLength ||
            !decodedSemanticSpanValid(entry.keyOffset, entry.keyLength) ||
            !decodedSemanticSpanValid(entry.valueOffset, entry.valueLength)) return Status::Invalid;
        for (size_t i = 0; i < count_; ++i) {
            if (decodedKeysEqual(entries_[i], entry)) return Status::Invalid;
        }
        entries_[count_++] = entry;
        return Status::Valid;
    }

    Status parseUrlEncoded() {
        if (!bytes_ && size_ != 0) return Status::Invalid;
        if (size_ == 0) return Status::Valid;
        size_t cursor = 0;
        while (cursor < size_) {
            const size_t pairStart = cursor;
            while (cursor < size_ && bytes_[cursor] != '&') ++cursor;
            const size_t pairEnd = cursor;
            if (pairEnd == pairStart) return Status::Invalid;

            size_t equals = pairStart;
            while (equals < pairEnd && bytes_[equals] != '=') ++equals;
            if (equals == pairStart || equals == pairEnd) return Status::Invalid;

            Entry entry;
            entry.keyOffset = pairStart;
            entry.keyLength = equals - pairStart;
            entry.valueOffset = equals + 1u;
            entry.valueLength = pairEnd - entry.valueOffset;
            const Status added = addEntry(entry);
            if (added != Status::Valid) return added;
            if (cursor < size_) {
                ++cursor;
                if (cursor == size_) return Status::Invalid;
            }
        }
        return Status::Valid;
    }

    Status parseMultipart() {
        if (!bytes_ || !multipartBoundary_ || multipartBoundaryLength_ == 0 ||
            multipartBoundaryLength_ > 70u || size_ == 0) return Status::Invalid;
        for (size_t i = 0; i < multipartBoundaryLength_; ++i) {
            const char value = multipartBoundary_[i];
            const bool valid = (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
                               (value >= 'a' && value <= 'z') || value == '\'' || value == '(' ||
                               value == ')' || value == '+' || value == '_' || value == ',' ||
                               value == '-' || value == '.' || value == '/' || value == ':' ||
                               value == '=' || value == '?';
            if (!valid) return Status::Invalid;
        }

        size_t cursor = 0;
        if (size_ < multipartBoundaryLength_ + 4u || !bytesEqual(cursor, "--", 2u) ||
            !bytesEqual(cursor + 2u, multipartBoundary_, multipartBoundaryLength_) ||
            !bytesEqual(cursor + 2u + multipartBoundaryLength_, "\r\n", 2u)) return Status::Invalid;
        cursor += 4u + multipartBoundaryLength_;

        constexpr char kDisposition[] = "Content-Disposition: form-data; name=\"";
        while (cursor < size_) {
            const size_t headerLength = sizeof(kDisposition) - 1u;
            if (!bytesEqualIgnoreCase(cursor, kDisposition, headerLength)) return Status::Invalid;
            const size_t keyStart = cursor + headerLength;
            const uint8_t* const quote = findRaw(keyStart, "\"\r\n\r\n", 5u);
            if (!quote) return Status::Invalid;
            const size_t keyEnd = static_cast<size_t>(quote - bytes_);
            if (keyEnd == keyStart) return Status::Invalid;
            if (keyEnd - keyStart > 64u) return Status::Invalid;
            for (size_t i = keyStart; i < keyEnd; ++i) {
                const uint8_t value = bytes_[i];
                if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                      (value >= '0' && value <= '9') || value == '_')) return Status::Invalid;
            }
            const size_t valueStart = keyEnd + 5u;

            const uint8_t* delimiter = nullptr;
            for (size_t search = valueStart; search + 4u + multipartBoundaryLength_ <= size_; ++search) {
                if (bytesEqual(search, "\r\n--", 4u) &&
                    bytesEqual(search + 4u, multipartBoundary_, multipartBoundaryLength_)) {
                    delimiter = bytes_ + search;
                    break;
                }
            }
            if (!delimiter) return Status::Invalid;
            const size_t valueEnd = static_cast<size_t>(delimiter - bytes_);
            Entry entry;
            entry.keyOffset = keyStart;
            entry.keyLength = keyEnd - keyStart;
            entry.valueOffset = valueStart;
            entry.valueLength = valueEnd - valueStart;
            const Status added = addEntry(entry);
            if (added != Status::Valid) return added;

            cursor = valueEnd + 4u + multipartBoundaryLength_;
            if (bytesEqual(cursor, "--\r\n", 4u)) {
                cursor += 4u;
                return cursor == size_ ? Status::Valid : Status::Invalid;
            }
            if (!bytesEqual(cursor, "\r\n", 2u)) return Status::Invalid;
            cursor += 2u;
        }
        return Status::Invalid;
    }

    Status parse() { return multipart_ ? parseMultipart() : parseUrlEncoded(); }

    const uint8_t* bytes_ = nullptr;
    size_t size_ = 0;
    const char* multipartBoundary_ = nullptr;
    size_t multipartBoundaryLength_ = 0;
    bool multipart_ = false;
    std::array<Entry, kMaxFields> entries_{};
    size_t count_ = 0;
    Status status_ = Status::Invalid;
};
