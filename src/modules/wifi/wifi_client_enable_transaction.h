#pragma once

namespace WifiClientEnableTransaction {

struct Runtime {
    void* ctx = nullptr;
    bool persistedEnabled = false;
    bool lifecycleAdmitted = false;
    bool (*admitStart)(void* ctx) = nullptr;
    bool (*attemptStart)(void* ctx) = nullptr;
    void (*rollbackFailedStart)(void* ctx) = nullptr;
    void (*commitEnabled)(void* ctx) = nullptr;
};

bool execute(const Runtime& runtime);

} // namespace WifiClientEnableTransaction
