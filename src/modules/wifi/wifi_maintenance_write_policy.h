#pragma once

// Request-shape policy for mutating maintenance API requests. The fixed
// header distinguishes intended WebUI writes; it is not authentication.
namespace WifiMaintenanceWritePolicy {

constexpr const char* kRequestShapeHeader = "X-V1Simple-Request";
constexpr const char* kRequestShapeValue = "maintenance-ui";

enum class Decision {
    Allow,
    RejectNotMaintenance,
    RejectHeader,
};

inline Decision evaluate(bool maintenanceBootMode, bool hasValidWriteHeader) {
    if (!maintenanceBootMode) {
        return Decision::RejectNotMaintenance;
    }
    if (!hasValidWriteHeader) {
        return Decision::RejectHeader;
    }
    return Decision::Allow;
}

template <typename Server, typename Resolver, typename Handler>
inline void dispatchStorageResolved(Server& server, bool& preAdmitted, Resolver&& resolve, Handler&& handler) {
    if (resolve()) {
        handler();
        return;
    }
    preAdmitted = false;
    server.send(503, "application/json",
                "{\"success\":false,\"error\":\"storage_transaction_recovery_pending\","
                "\"message\":\"Storage recovery is incomplete; retry this request\"}");
}

} // namespace WifiMaintenanceWritePolicy
