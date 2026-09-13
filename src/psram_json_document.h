#pragma once

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// Large backup documents must never consume the ESP32-S3's comparatively
// scarce internal SRAM when PSRAM is exhausted or unavailable.  Callers using
// this document fail closed through JsonDocument::overflowed().
namespace PsramJson {

inline constexpr uint32_t kCaps = MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM;

class Allocator : public ArduinoJson::Allocator {
  public:
    static Allocator& instance() {
        static Allocator allocator;
        return allocator;
    }

    void* allocate(size_t size) override { return heap_caps_malloc(size, kCaps); }

    void deallocate(void* ptr) override { heap_caps_free(ptr); }

    void* reallocate(void* ptr, size_t newSize) override {
        return heap_caps_realloc(ptr, newSize, kCaps);
    }
};

class Document : public JsonDocument {
  public:
    Document() : JsonDocument(&Allocator::instance()) {}
};

class Buffer {
  public:
    explicit Buffer(size_t size)
        : data_(size == 0 ? nullptr : static_cast<uint8_t*>(heap_caps_malloc(size, kCaps))), size_(size) {}
    ~Buffer() { heap_caps_free(data_); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    uint8_t* data() { return data_; }
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    explicit operator bool() const { return size_ == 0 || data_ != nullptr; }

  private:
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

} // namespace PsramJson
