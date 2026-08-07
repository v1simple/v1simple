#include "wifi_client_enable_transaction.h"

namespace WifiClientEnableTransaction {

bool execute(const Runtime& runtime) {
    if (!runtime.attemptStart || !runtime.rollbackFailedStart || !runtime.commitEnabled) {
        return false;
    }

    if (runtime.persistedEnabled && runtime.lifecycleAdmitted) {
        return true;
    }

    if (!runtime.attemptStart(runtime.ctx)) {
        runtime.rollbackFailedStart(runtime.ctx);
        return false;
    }

    if (!runtime.persistedEnabled) {
        runtime.commitEnabled(runtime.ctx);
    }
    return true;
}

} // namespace WifiClientEnableTransaction
