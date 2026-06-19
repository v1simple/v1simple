#pragma once

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

namespace WifiJson {

inline constexpr uint32_t kPsramCaps = MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM;
inline constexpr uint32_t kInternalCaps = MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL;

class Allocator : public ArduinoJson::Allocator {
public:
    static Allocator& instance() {
        static Allocator allocator;
        return allocator;
    }

    void* allocate(size_t size) override {
        void* ptr = heap_caps_malloc(size, kPsramCaps);
        if (ptr != nullptr) {
            return ptr;
        }
        return heap_caps_malloc(size, kInternalCaps);
    }

    void deallocate(void* ptr) override {
        heap_caps_free(ptr);
    }

    void* reallocate(void* ptr, size_t newSize) override {
        void* resized = heap_caps_realloc(ptr, newSize, kPsramCaps);
        if (resized != nullptr) {
            return resized;
        }
        return heap_caps_realloc(ptr, newSize, kInternalCaps);
    }
};

inline Allocator& allocator() {
    return Allocator::instance();
}

class Document : public JsonDocument {
public:
    Document()
        : JsonDocument(&Allocator::instance()) {}
};

}  // namespace WifiJson
