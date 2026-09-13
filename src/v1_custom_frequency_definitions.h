#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

struct V1CustomFrequencyDefinition {
    uint8_t index = 0;
    uint16_t lowerMHz = 0;
    uint16_t upperMHz = 0;
};

inline bool operator==(const V1CustomFrequencyDefinition& lhs,
                       const V1CustomFrequencyDefinition& rhs) {
    return lhs.index == rhs.index && lhs.lowerMHz == rhs.lowerMHz &&
           lhs.upperMHz == rhs.upperMHz;
}

// The detector protocol has a six-bit definition selector, so 64 entries is
// the complete representable set.  Keeping that bound in the value type makes
// profile parsing/copying, restore staging, migration, and Auto-Push admission
// allocation-free for the request-controlled definition collection.
class V1CustomFrequencyDefinitionList {
public:
    static constexpr size_t kCapacity = 64;
    using Storage = std::array<V1CustomFrequencyDefinition, kCapacity>;
    using iterator = Storage::iterator;
    using const_iterator = Storage::const_iterator;

    constexpr size_t size() const { return count_; }
    constexpr size_t capacity() const { return kCapacity; }
    constexpr bool empty() const { return count_ == 0; }

    void clear() { count_ = 0; }

    bool push_back(const V1CustomFrequencyDefinition& value) {
        if (count_ >= kCapacity) return false;
        values_[count_++] = value;
        return true;
    }

    template <typename Collection>
    bool assign(const Collection& source) {
        if (source.size() > kCapacity) return false;
        V1CustomFrequencyDefinitionList prepared;
        for (const V1CustomFrequencyDefinition& value : source) {
            if (!prepared.push_back(value)) return false;
        }
        *this = prepared;
        return true;
    }

    V1CustomFrequencyDefinition& operator[](size_t index) { return values_[index]; }
    const V1CustomFrequencyDefinition& operator[](size_t index) const { return values_[index]; }
    V1CustomFrequencyDefinition& back() { return values_[count_ - 1u]; }
    const V1CustomFrequencyDefinition& back() const { return values_[count_ - 1u]; }

    iterator begin() { return values_.begin(); }
    const_iterator begin() const { return values_.begin(); }
    const_iterator cbegin() const { return values_.cbegin(); }
    iterator end() { return values_.begin() + static_cast<std::ptrdiff_t>(count_); }
    const_iterator end() const {
        return values_.begin() + static_cast<std::ptrdiff_t>(count_);
    }
    const_iterator cend() const {
        return values_.cbegin() + static_cast<std::ptrdiff_t>(count_);
    }

private:
    Storage values_{};
    size_t count_ = 0;
};

inline bool operator==(const V1CustomFrequencyDefinitionList& lhs,
                       const V1CustomFrequencyDefinitionList& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
        if (!(lhs[index] == rhs[index])) return false;
    }
    return true;
}

inline bool operator!=(const V1CustomFrequencyDefinitionList& lhs,
                       const V1CustomFrequencyDefinitionList& rhs) {
    return !(lhs == rhs);
}
