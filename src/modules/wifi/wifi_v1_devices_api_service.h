#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <cstdint>
#include <vector>

#include "v1_devices.h"

namespace WifiV1DevicesApiService {

struct DeviceInfo {
    String address;
    String name;
    uint8_t defaultProfile = 0; // 0=none/global slot, 1..3=slot override
    bool connected = false;
};

struct Runtime {
    bool (*listDevices)(std::vector<DeviceInfo>& output, void* ctx) = nullptr;
    void* listDevicesCtx = nullptr;
    V1DeviceMutationResult (*setDeviceName)(const String& address, const String& name, void* ctx) = nullptr;
    void* setDeviceNameCtx = nullptr;
    V1DeviceMutationResult (*setDeviceDefaultProfile)(const String& address, uint8_t profile,
                                                      void* ctx) = nullptr;
    void* setDeviceDefaultProfileCtx = nullptr;
    V1DeviceMutationResult (*deleteDevice)(const String& address, void* ctx) = nullptr;
    void* deleteDeviceCtx = nullptr;
};

void handleApiDevicesList(WebServer& server, const Runtime& runtime);

void handleApiDeviceNameSave(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                             void* rateLimitCtx);
void handleApiDeviceNameSaveBody(WebServer& server, const Runtime& runtime,
                                 const uint8_t* body, size_t bodySize,
                                 bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                                 const char* multipartBoundary = nullptr, size_t multipartBoundarySize = 0);

void handleApiDeviceProfileSave(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                                void* rateLimitCtx);
void handleApiDeviceProfileSaveBody(WebServer& server, const Runtime& runtime,
                                    const uint8_t* body, size_t bodySize,
                                    bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                                    const char* multipartBoundary = nullptr, size_t multipartBoundarySize = 0);

void handleApiDeviceDelete(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                           void* rateLimitCtx);
void handleApiDeviceDeleteBody(WebServer& server, const Runtime& runtime,
                               const uint8_t* body, size_t bodySize,
                               bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                               const char* multipartBoundary = nullptr, size_t multipartBoundarySize = 0);

} // namespace WifiV1DevicesApiService
