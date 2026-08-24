#pragma once

#include <stdint.h>

// Owns late-loop BLE drain + yield + loop-duration finalization.
class LoopTailModule {
  public:
    struct Providers {
        uint32_t (*loopMicrosUs)(void* ctx) = nullptr;
        void* loopMicrosContext = nullptr;

        void (*runBleDrain)(void* ctx) = nullptr;
        void* bleDrainContext = nullptr;
        void (*yieldOneTick)(void* ctx) = nullptr;
        void* yieldContext = nullptr;
    };

    bool begin(const Providers& hooks);
    uint32_t process(bool bleBackpressure, uint32_t loopStartUs, bool forceBleDrain = false);

  private:
    Providers providers{};
};
