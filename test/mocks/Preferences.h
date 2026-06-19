#pragma once

#include "Arduino.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

enum PreferenceType : uint8_t {
    PT_INVALID = 0,
    PT_I8,
    PT_U8,
    PT_I16,
    PT_U16,
    PT_I32,
    PT_U32,
    PT_I64,
    PT_U64,
    PT_STR,
    PT_BLOB,
    PT_BOOL,
};

namespace mock_preferences {

struct Entry {
    PreferenceType type = PT_INVALID;
    std::variant<int64_t, uint64_t, std::string, std::vector<uint8_t>, bool> value = int64_t{0};
};

using NamespaceStore = std::unordered_map<std::string, Entry>;
using Store = std::unordered_map<std::string, NamespaceStore>;

inline Store& store() {
    static Store g_store;
    return g_store;
}

inline bool& failWrites() {
    static bool g_failWrites = false;
    return g_failWrites;
}

inline void reset() {
    store().clear();
    failWrites() = false;
}

inline void set_fail_writes(bool enabled) {
    failWrites() = enabled;
}

inline NamespaceStore& ensureNamespace(const std::string& name) {
    return store()[name];
}

inline const NamespaceStore* findNamespace(const std::string& name) {
    const auto it = store().find(name);
    return it == store().end() ? nullptr : &it->second;
}

inline const Entry* find(const std::string& ns, const std::string& key) {
    const NamespaceStore* nsStore = findNamespace(ns);
    if (!nsStore) {
        return nullptr;
    }
    const auto it = nsStore->find(key);
    return it == nsStore->end() ? nullptr : &it->second;
}

inline bool namespaceHasKey(const char* ns, const char* key) {
    if (!ns || !key) {
        return false;
    }
    return find(ns, key) != nullptr;
}

inline String getString(const char* ns, const char* key, const char* defaultValue = "") {
    const Entry* entry = (ns && key) ? find(ns, key) : nullptr;
    if (!entry || entry->type != PT_STR) {
        return String(defaultValue ? defaultValue : "");
    }
    return String(std::get<std::string>(entry->value).c_str());
}

inline int64_t getSigned(const char* ns, const char* key, int64_t defaultValue = 0) {
    const Entry* entry = (ns && key) ? find(ns, key) : nullptr;
    if (!entry) {
        return defaultValue;
    }
    switch (entry->type) {
        case PT_I8:
        case PT_I16:
        case PT_I32:
        case PT_I64:
            return std::get<int64_t>(entry->value);
        case PT_U8:
        case PT_U16:
        case PT_U32:
        case PT_U64:
            return static_cast<int64_t>(std::get<uint64_t>(entry->value));
        case PT_BOOL:
            return std::get<bool>(entry->value) ? 1 : 0;
        default:
            return defaultValue;
    }
}

inline uint64_t getUnsigned(const char* ns, const char* key, uint64_t defaultValue = 0) {
    const Entry* entry = (ns && key) ? find(ns, key) : nullptr;
    if (!entry) {
        return defaultValue;
    }
    switch (entry->type) {
        case PT_I8:
        case PT_I16:
        case PT_I32:
        case PT_I64:
            return static_cast<uint64_t>(std::max<int64_t>(0, std::get<int64_t>(entry->value)));
        case PT_U8:
        case PT_U16:
        case PT_U32:
        case PT_U64:
            return std::get<uint64_t>(entry->value);
        case PT_BOOL:
            return std::get<bool>(entry->value) ? 1u : 0u;
        default:
            return defaultValue;
    }
}

inline std::vector<uint8_t> getBlob(const char* ns, const char* key) {
    const Entry* entry = (ns && key) ? find(ns, key) : nullptr;
    if (!entry || entry->type != PT_BLOB) {
        return {};
    }
    return std::get<std::vector<uint8_t>>(entry->value);
}

}  // namespace mock_preferences

class Preferences {
public:
    Preferences() = default;

    bool begin(const char* name, bool readOnly = false, const char* /*partition_label*/ = nullptr) {
        if (!name || name[0] == '\0') {
            started_ = false;
            namespaceName_.clear();
            return false;
        }
        namespaceName_ = name;
        readOnly_ = readOnly;
        started_ = true;
        mock_preferences::ensureNamespace(namespaceName_);
        return true;
    }

    void end() {
        started_ = false;
        readOnly_ = false;
        namespaceName_.clear();
    }

    bool clear() {
        if (!started_ || readOnly_) {
            return false;
        }
        mock_preferences::ensureNamespace(namespaceName_).clear();
        return true;
    }

    bool remove(const char* key) {
        if (!started_ || readOnly_ || !key) {
            return false;
        }
        return mock_preferences::ensureNamespace(namespaceName_).erase(key) > 0;
    }

    bool isKey(const char* key) const {
        if (!started_ || !key) {
            return false;
        }
        return mock_preferences::namespaceHasKey(namespaceName_.c_str(), key);
    }

    PreferenceType getType(const char* key) const {
        const mock_preferences::Entry* entry =
            (started_ && key) ? mock_preferences::find(namespaceName_, key) : nullptr;
        return entry ? entry->type : PT_INVALID;
    }

    bool getBool(const char* key, bool defaultValue = false) const {
        const mock_preferences::Entry* entry =
            (started_ && key) ? mock_preferences::find(namespaceName_, key) : nullptr;
        if (!entry) {
            return defaultValue;
        }
        if (entry->type == PT_BOOL) {
            return std::get<bool>(entry->value);
        }
        if (entry->type == PT_STR || entry->type == PT_BLOB) {
            return defaultValue;
        }
        return mock_preferences::getUnsigned(namespaceName_.c_str(), key, defaultValue ? 1u : 0u) != 0;
    }

    int8_t getChar(const char* key, int8_t defaultValue = 0) const {
        return static_cast<int8_t>(mock_preferences::getSigned(namespaceName_.c_str(), key, defaultValue));
    }

    uint8_t getUChar(const char* key, uint8_t defaultValue = 0) const {
        return static_cast<uint8_t>(mock_preferences::getUnsigned(namespaceName_.c_str(), key, defaultValue));
    }

    int16_t getShort(const char* key, int16_t defaultValue = 0) const {
        return static_cast<int16_t>(mock_preferences::getSigned(namespaceName_.c_str(), key, defaultValue));
    }

    uint16_t getUShort(const char* key, uint16_t defaultValue = 0) const {
        return static_cast<uint16_t>(mock_preferences::getUnsigned(namespaceName_.c_str(), key, defaultValue));
    }

    int32_t getInt(const char* key, int32_t defaultValue = 0) const {
        return static_cast<int32_t>(mock_preferences::getSigned(namespaceName_.c_str(), key, defaultValue));
    }

    uint32_t getUInt(const char* key, uint32_t defaultValue = 0) const {
        return static_cast<uint32_t>(mock_preferences::getUnsigned(namespaceName_.c_str(), key, defaultValue));
    }

    long getLong(const char* key, long defaultValue = 0) const {
        return static_cast<long>(mock_preferences::getSigned(namespaceName_.c_str(), key, defaultValue));
    }

    unsigned long getULong(const char* key, unsigned long defaultValue = 0) const {
        return static_cast<unsigned long>(
            mock_preferences::getUnsigned(namespaceName_.c_str(), key, defaultValue));
    }

    int64_t getLong64(const char* key, int64_t defaultValue = 0) const {
        return mock_preferences::getSigned(namespaceName_.c_str(), key, defaultValue);
    }

    String getString(const char* key, const String& defaultValue = String()) const {
        return mock_preferences::getString(namespaceName_.c_str(), key, defaultValue.c_str());
    }

    size_t getBytes(const char* key, void* buffer, size_t maxLen) const {
        if (!started_ || !key || !buffer) {
            return 0;
        }
        const std::vector<uint8_t> blob = mock_preferences::getBlob(namespaceName_.c_str(), key);
        const size_t copyLen = std::min(maxLen, blob.size());
        if (copyLen > 0) {
            std::memcpy(buffer, blob.data(), copyLen);
        }
        return copyLen;
    }

    size_t putBool(const char* key, bool value) {
        return storeScalar(key, PT_BOOL, value, 1);
    }

    size_t putChar(const char* key, int8_t value) {
        return storeScalar(key, PT_I8, static_cast<int64_t>(value), sizeof(value));
    }

    size_t putUChar(const char* key, uint8_t value) {
        return storeScalar(key, PT_U8, static_cast<uint64_t>(value), sizeof(value));
    }

    size_t putShort(const char* key, int16_t value) {
        return storeScalar(key, PT_I16, static_cast<int64_t>(value), sizeof(value));
    }

    size_t putUShort(const char* key, uint16_t value) {
        return storeScalar(key, PT_U16, static_cast<uint64_t>(value), sizeof(value));
    }

    size_t putInt(const char* key, int32_t value) {
        return storeScalar(key, PT_I32, static_cast<int64_t>(value), sizeof(value));
    }

    size_t putUInt(const char* key, uint32_t value) {
        return storeScalar(key, PT_U32, static_cast<uint64_t>(value), sizeof(value));
    }

    size_t putLong(const char* key, long value) {
        return storeScalar(key, PT_I32, static_cast<int64_t>(value), sizeof(value));
    }

    size_t putULong(const char* key, unsigned long value) {
        return storeScalar(key, PT_U32, static_cast<uint64_t>(value), sizeof(value));
    }

    size_t putLong64(const char* key, int64_t value) {
        return storeScalar(key, PT_I64, value, sizeof(value));
    }

    size_t putString(const char* key, const String& value) {
        return putString(key, value.c_str());
    }

    size_t putString(const char* key, const char* value) {
        if (!started_ || readOnly_ || !key || !value || mock_preferences::failWrites()) {
            return 0;
        }
        mock_preferences::Entry entry;
        entry.type = PT_STR;
        entry.value = std::string(value);
        mock_preferences::ensureNamespace(namespaceName_)[key] = std::move(entry);
        return std::strlen(value);
    }

    size_t putBytes(const char* key, const void* value, size_t len) {
        if (!started_ || readOnly_ || !key || (!value && len > 0) ||
            mock_preferences::failWrites()) {
            return 0;
        }
        mock_preferences::Entry entry;
        entry.type = PT_BLOB;
        const uint8_t* bytes = static_cast<const uint8_t*>(value);
        entry.value = std::vector<uint8_t>(bytes, bytes + len);
        mock_preferences::ensureNamespace(namespaceName_)[key] = std::move(entry);
        return len;
    }

private:
    template <typename Value>
    size_t storeScalar(const char* key, PreferenceType type, Value value, size_t writtenBytes) {
        if (!started_ || readOnly_ || !key || mock_preferences::failWrites()) {
            return 0;
        }
        mock_preferences::Entry entry;
        entry.type = type;
        entry.value = value;
        mock_preferences::ensureNamespace(namespaceName_)[key] = std::move(entry);
        return writtenBytes;
    }

    bool started_ = false;
    bool readOnly_ = false;
    std::string namespaceName_;
};
