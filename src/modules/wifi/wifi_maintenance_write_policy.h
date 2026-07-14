#pragma once

// Pure authorization policy for mutating maintenance API requests.
// Keep transport concerns (headers, logging, and HTTP responses) in the caller.
namespace WifiMaintenanceWritePolicy {

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

} // namespace WifiMaintenanceWritePolicy
