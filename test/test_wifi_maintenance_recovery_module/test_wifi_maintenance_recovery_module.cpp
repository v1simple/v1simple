#include <unity.h>

#include <string>

#include "../mocks/WebServer.h"
#include "../../src/modules/wifi/wifi_ap_lifecycle_policy.h"
#include "../../src/modules/wifi/wifi_maintenance_interface_policy.h"
#include "../../src/modules/wifi/wifi_maintenance_link_policy.h"
#include "../../src/modules/wifi/wifi_saved_network_mutation_policy.h"
#include "../../src/modules/wifi/wifi_setup_network_policy.h"
#include "../../src/modules/wifi/wifi_maintenance_http_preflight.h"
#include "../../src/modules/wifi/wifi_maintenance_recovery_module.cpp"

// Regression coverage for the maintenance-boot WiFi recovery policy.
//
// Escaped-bug class (2026-07): the maintenance AP could stop (failed start at
// entry, emergency low-SRAM stop) with no restart path, leaving a WiFi-dead
// session until the maintenance timeout rebooted the device. The recovery
// module is the loop's guarantee that a down service gets bounded, scheduled
// restart attempts, and that an active service resets the schedule.

static WifiMaintenanceRecoveryInput makeInput(bool serviceReachable, unsigned long nowMs) {
    WifiMaintenanceRecoveryInput input;
    input.maintenanceBootActive = true;
    input.wifiServiceReachable = serviceReachable;
    input.nowMs = nowMs;
    return input;
}

void test_service_active_never_attempts() {
    WifiMaintenanceRecoveryModule module;
    for (unsigned long t = 0; t <= 120000UL; t += 1000UL) {
        const WifiMaintenanceRecoveryResult result = module.evaluate(makeInput(true, t));
        TEST_ASSERT_FALSE(result.attemptRestart);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, module.attemptCount());
}

void test_outside_maintenance_never_attempts() {
    WifiMaintenanceRecoveryModule module;
    WifiMaintenanceRecoveryInput input = makeInput(false, 10000UL);
    input.maintenanceBootActive = false;
    const WifiMaintenanceRecoveryResult result = module.evaluate(input);
    TEST_ASSERT_FALSE(result.attemptRestart);
}

void test_first_attempt_waits_for_first_retry_delay() {
    WifiMaintenanceRecoveryModule module;
    // First down observation anchors the schedule and never fires.
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 1000UL)).attemptRestart);
    // Just before the delay elapses: still waiting.
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 1000UL + WifiMaintenanceRecoveryModule::kFirstRetryDelayMs - 1))
                          .attemptRestart);
    // Exactly at the delay: fire attempt #1.
    const WifiMaintenanceRecoveryResult result =
        module.evaluate(makeInput(false, 1000UL + WifiMaintenanceRecoveryModule::kFirstRetryDelayMs));
    TEST_ASSERT_TRUE(result.attemptRestart);
    TEST_ASSERT_EQUAL_UINT32(1u, result.attemptNumber);
}

void test_repeat_attempts_follow_retry_interval() {
    WifiMaintenanceRecoveryModule module;
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 0UL)).attemptRestart);
    const unsigned long firstAttemptMs = WifiMaintenanceRecoveryModule::kFirstRetryDelayMs;
    TEST_ASSERT_TRUE(module.evaluate(makeInput(false, firstAttemptMs)).attemptRestart);

    // Between attempts: quiet.
    TEST_ASSERT_FALSE(
        module.evaluate(makeInput(false, firstAttemptMs + WifiMaintenanceRecoveryModule::kRetryIntervalMs - 1))
            .attemptRestart);

    // At the interval: attempt #2, and the schedule re-anchors on it.
    const unsigned long secondAttemptMs = firstAttemptMs + WifiMaintenanceRecoveryModule::kRetryIntervalMs;
    const WifiMaintenanceRecoveryResult second = module.evaluate(makeInput(false, secondAttemptMs));
    TEST_ASSERT_TRUE(second.attemptRestart);
    TEST_ASSERT_EQUAL_UINT32(2u, second.attemptNumber);

    const WifiMaintenanceRecoveryResult third =
        module.evaluate(makeInput(false, secondAttemptMs + WifiMaintenanceRecoveryModule::kRetryIntervalMs));
    TEST_ASSERT_TRUE(third.attemptRestart);
    TEST_ASSERT_EQUAL_UINT32(3u, third.attemptNumber);
}

void test_service_recovery_resets_schedule() {
    WifiMaintenanceRecoveryModule module;
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 0UL)).attemptRestart);
    TEST_ASSERT_TRUE(
        module.evaluate(makeInput(false, WifiMaintenanceRecoveryModule::kFirstRetryDelayMs)).attemptRestart);

    // Service comes back: full reset.
    TEST_ASSERT_FALSE(module.evaluate(makeInput(true, 60000UL)).attemptRestart);
    TEST_ASSERT_EQUAL_UINT32(0u, module.attemptCount());

    // Goes down again: the first-retry delay applies afresh from the new
    // anchor, not from stale state.
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 70000UL)).attemptRestart);
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 70000UL + WifiMaintenanceRecoveryModule::kFirstRetryDelayMs - 1))
                          .attemptRestart);
    const WifiMaintenanceRecoveryResult result =
        module.evaluate(makeInput(false, 70000UL + WifiMaintenanceRecoveryModule::kFirstRetryDelayMs));
    TEST_ASSERT_TRUE(result.attemptRestart);
    TEST_ASSERT_EQUAL_UINT32(1u, result.attemptNumber);
}

void test_now_zero_anchor_is_preserved_across_repeated_ticks() {
    WifiMaintenanceRecoveryModule module;
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 0UL)).attemptRestart);
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 0UL)).attemptRestart);
    TEST_ASSERT_FALSE(
        module.evaluate(makeInput(false, WifiMaintenanceRecoveryModule::kFirstRetryDelayMs - 1)).attemptRestart);
    TEST_ASSERT_TRUE(
        module.evaluate(makeInput(false, WifiMaintenanceRecoveryModule::kFirstRetryDelayMs)).attemptRestart);
}

void test_rollover_safe_delta() {
    WifiMaintenanceRecoveryModule module;
    // Anchor just before unsigned rollover; the delay elapses across it.
    const unsigned long nearMax = static_cast<unsigned long>(-1) - 1000UL;
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, nearMax)).attemptRestart);
    const unsigned long afterRollover = WifiMaintenanceRecoveryModule::kFirstRetryDelayMs - 1000UL;
    TEST_ASSERT_TRUE(module.evaluate(makeInput(false, afterRollover)).attemptRestart);
}

void test_ap_bringup_abort_clears_stale_interface_state_for_all_consumers() {
    bool apInterfaceEnabled = true;

    apInterfaceEnabled = WifiApLifecyclePolicy::afterBringupAbort(apInterfaceEnabled);

    TEST_ASSERT_FALSE(apInterfaceEnabled);
    TEST_ASSERT_FALSE(WifiApLifecyclePolicy::isSetupModeActive(true, apInterfaceEnabled));
    TEST_ASSERT_FALSE(WifiApLifecyclePolicy::shouldDisableInterfaceOnStop(false, apInterfaceEnabled));
}

void test_maintenance_with_saved_credentials_uses_ap_sta_auto_connect() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiSetupNetworkPolicy::Mode::ApSta),
                          static_cast<int>(WifiSetupNetworkPolicy::select(true, true)));
    TEST_ASSERT_TRUE(WifiSetupNetworkPolicy::usesSta(true, true));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(WifiSetupNetworkPolicy::SavedNetworkStart::MaintenanceAutoConnect),
        static_cast<int>(WifiSetupNetworkPolicy::selectSavedNetworkStart(true, true)));
}

void test_normal_setup_preserves_saved_sta_connectivity() {
    TEST_ASSERT_TRUE(WifiSetupNetworkPolicy::usesSta(false, true));
    TEST_ASSERT_FALSE(WifiSetupNetworkPolicy::usesSta(false, false));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiSetupNetworkPolicy::SavedNetworkStart::DirectConnect),
                          static_cast<int>(WifiSetupNetworkPolicy::selectSavedNetworkStart(false, true)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiSetupNetworkPolicy::SavedNetworkStart::None),
                          static_cast<int>(WifiSetupNetworkPolicy::selectSavedNetworkStart(true, false)));
}

void test_maintenance_auto_connect_retries_failed_storage_resolution_then_starts() {
    bool storageResolved = false;
    int resolveCalls = 0;
    int retryCalls = 0;
    int scanCalls = 0;
    const auto attempt = [&]() {
        return WifiSetupNetworkPolicy::startMaintenanceAutoConnect(
            [&]() {
                ++resolveCalls;
                return storageResolved;
            },
            [&]() { ++retryCalls; }, [&]() {
                ++scanCalls;
                return true;
            });
    };

    TEST_ASSERT_FALSE(attempt());
    TEST_ASSERT_EQUAL_INT(1, resolveCalls);
    TEST_ASSERT_EQUAL_INT(1, retryCalls);
    TEST_ASSERT_EQUAL_INT(0, scanCalls);

    storageResolved = true;
    TEST_ASSERT_TRUE(attempt());
    TEST_ASSERT_EQUAL_INT(2, resolveCalls);
    TEST_ASSERT_EQUAL_INT(1, retryCalls);
    TEST_ASSERT_EQUAL_INT(1, scanCalls);
}

void test_maintenance_http_interface_admission_allows_only_initialized_ap_destination() {
    constexpr uint32_t apIp = 0x0523A8C0u;
    constexpr uint32_t staIp = 0x6401A8C0u;

    TEST_ASSERT_TRUE(WifiMaintenanceInterfacePolicy::allows(apIp, apIp, staIp));
    TEST_ASSERT_FALSE(WifiMaintenanceInterfacePolicy::allows(staIp, apIp, staIp));
    TEST_ASSERT_FALSE(WifiMaintenanceInterfacePolicy::allows(0, apIp, staIp));
    TEST_ASSERT_FALSE(WifiMaintenanceInterfacePolicy::allows(apIp, 0, staIp));
    TEST_ASSERT_FALSE(WifiMaintenanceInterfacePolicy::allows(apIp, apIp, apIp));
    TEST_ASSERT_TRUE(WifiMaintenanceInterfacePolicy::hasAddressCollision(apIp, apIp));
    TEST_ASSERT_FALSE(WifiMaintenanceInterfacePolicy::hasAddressCollision(apIp, 0));
}

void test_physical_auto_reconnect_is_reconciled_into_app_state() {
    WifiMaintenanceLinkPolicy::Input input;
    input.physicalConnected = true;
    input.clientEnabled = true;
    input.hasSavedCandidates = true;

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(WifiMaintenanceLinkPolicy::Decision::ReconcilePhysicalConnection),
        static_cast<int>(WifiMaintenanceLinkPolicy::evaluate(input)));
}

void test_true_link_loss_waits_then_restarts_candidate_scan() {
    WifiMaintenanceLinkPolicy::Input input;
    input.clientEnabled = true;
    input.hasSavedCandidates = true;
    input.retryAtMs = 5000;
    input.nowMs = 4999;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMaintenanceLinkPolicy::Decision::None),
                          static_cast<int>(WifiMaintenanceLinkPolicy::evaluate(input)));

    input.nowMs = 5000;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMaintenanceLinkPolicy::Decision::StartCandidateScan),
                          static_cast<int>(WifiMaintenanceLinkPolicy::evaluate(input)));

    input.autoConnectActive = true;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMaintenanceLinkPolicy::Decision::None),
                          static_cast<int>(WifiMaintenanceLinkPolicy::evaluate(input)));
}

void test_explicit_disconnect_zero_deadline_never_auto_rejoins() {
    WifiMaintenanceLinkPolicy::Input input;
    input.clientEnabled = true; // persisted preference remains unchanged
    input.hasSavedCandidates = true;
    input.retryAtMs = 0; // explicit session-level disconnect
    input.nowMs = 0xFFFFFFFFu;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMaintenanceLinkPolicy::Decision::None),
                          static_cast<int>(WifiMaintenanceLinkPolicy::evaluate(input)));
}

void test_explicit_disconnect_rejects_a_late_physical_connection() {
    WifiMaintenanceLinkPolicy::Input input;
    input.physicalConnected = true;
    input.clientEnabled = true; // persisted preference remains unchanged
    input.hasSavedCandidates = true;
    input.autoJoinSuppressed = true;

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(WifiMaintenanceLinkPolicy::Decision::RejectSuppressedPhysicalConnection),
        static_cast<int>(WifiMaintenanceLinkPolicy::evaluate(input)));
}

void test_equal_ip_collision_forces_physical_sta_disconnect() {
    TEST_ASSERT_TRUE(WifiMaintenanceLinkPolicy::shouldDisconnectAddressCollision(true, true));
    TEST_ASSERT_FALSE(WifiMaintenanceLinkPolicy::shouldDisconnectAddressCollision(false, true));
    TEST_ASSERT_FALSE(WifiMaintenanceLinkPolicy::shouldDisconnectAddressCollision(true, false));
}

void test_link_retry_deadline_is_rollover_safe() {
    WifiMaintenanceLinkPolicy::Input input;
    input.clientEnabled = true;
    input.hasSavedCandidates = true;
    input.retryAtMs = 3;
    input.nowMs = 0xFFFFFFFEu;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMaintenanceLinkPolicy::Decision::None),
                          static_cast<int>(WifiMaintenanceLinkPolicy::evaluate(input)));
    input.nowMs = 3;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMaintenanceLinkPolicy::Decision::StartCandidateScan),
                          static_cast<int>(WifiMaintenanceLinkPolicy::evaluate(input)));
}

void test_failed_saved_network_persistence_has_no_runtime_side_effects() {
    const WifiSavedNetworkMutationPolicy::Decision decision = WifiSavedNetworkMutationPolicy::evaluate(
        {false, false, 1, 1, 1, true});
    TEST_ASSERT_FALSE(decision.cancelMaintenanceAutoActivity);
    TEST_ASSERT_FALSE(decision.disconnectTrackedActivity);
}

void test_committed_mutation_cancels_old_auto_activity_and_only_affected_link() {
    WifiSavedNetworkMutationPolicy::Decision decision =
        WifiSavedNetworkMutationPolicy::evaluate({true, false, 1, 0, 1, true});
    TEST_ASSERT_TRUE(decision.cancelMaintenanceAutoActivity);
    TEST_ASSERT_TRUE(decision.disconnectTrackedActivity);
    TEST_ASSERT_TRUE(decision.scheduleReplacementScan);

    decision = WifiSavedNetworkMutationPolicy::evaluate({true, false, 1, 0, -1, false});
    TEST_ASSERT_FALSE(decision.cancelMaintenanceAutoActivity);
    TEST_ASSERT_FALSE(decision.disconnectTrackedActivity);

    decision = WifiSavedNetworkMutationPolicy::evaluate({true, true, -1, 0, -1, false});
    TEST_ASSERT_TRUE(decision.disconnectTrackedActivity);
}

void test_scan_mutation_restarts_but_unrelated_connecting_mutation_is_preserved() {
    WifiSavedNetworkMutationPolicy::Decision decision =
        WifiSavedNetworkMutationPolicy::evaluate({true, false, 1, -1, -1, true, false});
    TEST_ASSERT_TRUE(decision.cancelMaintenanceAutoActivity);
    TEST_ASSERT_FALSE(decision.disconnectTrackedActivity);
    TEST_ASSERT_TRUE(decision.scheduleReplacementScan);

    decision = WifiSavedNetworkMutationPolicy::evaluate({true, false, 1, -1, 0, false, true});
    TEST_ASSERT_FALSE(decision.cancelMaintenanceAutoActivity);
    TEST_ASSERT_FALSE(decision.disconnectTrackedActivity);
    TEST_ASSERT_FALSE(decision.scheduleReplacementScan);

    decision = WifiSavedNetworkMutationPolicy::evaluate({true, false, 0, -1, 0, false, true});
    TEST_ASSERT_TRUE(decision.cancelMaintenanceAutoActivity);
    TEST_ASSERT_TRUE(decision.disconnectTrackedActivity);
    TEST_ASSERT_TRUE(decision.scheduleReplacementScan);
}

template <size_t N>
static void assertPreflightDecision(const char (&request)[N], const bool maintenanceBootMode,
                                    const WifiMaintenanceHttpPreflight::Decision expected) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected),
                          static_cast<int>(
                              WifiMaintenanceHttpPreflight::evaluate(request, N - 1, maintenanceBootMode)));
}

static void assertPreflightDecision(const std::string& request, const bool maintenanceBootMode,
                                    const WifiMaintenanceHttpPreflight::Decision expected) {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(expected),
        static_cast<int>(WifiMaintenanceHttpPreflight::evaluate(
            request.data(), request.size(), maintenanceBootMode)));
}

void test_http_preflight_allows_bounded_legacy_multipart_posts() {
    using WifiMaintenanceHttpPreflight::Decision;
    const char* const paths[] = {
        "/api/device/settings",
        "/api/obd/devices/name",
        "/api/autopush/activate",
        "/api/autopush/slot",
        "/api/v1/devices/name",
        "/api/v1/devices/profile",
        "/api/v1/devices/delete",
    };
    for (const char* path : paths) {
        const std::string request =
            std::string("POST ") + path + " HTTP/1.1\r\n"
            "X-V1Simple-Request: maintenance-ui\r\n"
            "Content-Type: multipart/form-data; boundary=legacy\r\n"
            "Content-Length: 120\r\n\r\n";
        assertPreflightDecision(request, true, Decision::AllowBodyParsing);
    }

    const char oversized[] =
        "POST /api/device/settings HTTP/1.1\r\n"
        "X-V1Simple-Request: maintenance-ui\r\n"
        "Content-Type: multipart/form-data; boundary=legacy\r\n"
        "Content-Length: 4097\r\n\r\n";
    assertPreflightDecision(oversized, true, Decision::RejectTooLarge);

    const char wrongMethod[] =
        "PUT /api/device/settings HTTP/1.1\r\n"
        "X-V1Simple-Request: maintenance-ui\r\n"
        "Content-Type: multipart/form-data; boundary=legacy\r\n"
        "Content-Length: 120\r\n\r\n";
    assertPreflightDecision(wrongMethod, true, Decision::RejectMultipart);

    const char missingBoundary[] =
        "POST /api/device/settings HTTP/1.1\r\n"
        "X-V1Simple-Request: maintenance-ui\r\n"
        "Content-Type: multipart/form-data\r\n"
        "Content-Length: 120\r\n\r\n";
    assertPreflightDecision(missingBoundary, true, Decision::RejectMultipart);
}

void test_http_preflight_rejects_multipart_before_framework_parser() {
    using WifiMaintenanceHttpPreflight::Decision;
    const char request[] =
        "POST /api/settings/restore HTTP/1.1\r\n"
        "Host: 192.168.4.1\r\n"
        "X-V1Simple-Request: maintenance-ui\r\n"
        "Content-Type: multipart/form-data; boundary=abc\r\n"
        "Content-Length: 120\r\n\r\n";
    assertPreflightDecision(request, true, Decision::RejectMultipart);

    const char nonLegacyRoute[] =
        "POST /api/audio/settings HTTP/1.1\r\n"
        "X-V1Simple-Request: maintenance-ui\r\n"
        "Content-Type: multipart/form-data; boundary=abc\r\n"
        "Content-Length: 120\r\n\r\n";
    assertPreflightDecision(nonLegacyRoute, true, Decision::RejectMultipart);
}

void test_http_preflight_rejects_wrong_shape_and_nonmaintenance_before_body() {
    using WifiMaintenanceHttpPreflight::Decision;
    const char missingShape[] =
        "POST /api/device/settings HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}";
    assertPreflightDecision(missingShape, true, Decision::RejectForbidden);

    const char invalidShape[] =
        "POST /api/device/settings HTTP/1.1\r\n"
        "X-V1Simple-Request: browser\r\nContent-Length: 0\r\n\r\n";
    assertPreflightDecision(invalidShape, true, Decision::RejectForbidden);

    const char duplicateShape[] =
        "POST /api/device/settings HTTP/1.1\r\n"
        "X-V1Simple-Request: maintenance-ui\r\n"
        "X-V1Simple-Request: maintenance-ui\r\nContent-Length: 0\r\n\r\n";
    assertPreflightDecision(duplicateShape, true, Decision::RejectForbidden);

    const char validShape[] =
        "POST /api/device/settings HTTP/1.1\r\n"
        "X-V1Simple-Request: maintenance-ui\r\nContent-Length: 0\r\n\r\n";
    assertPreflightDecision(validShape, false, Decision::RejectForbidden);
}

void test_http_preflight_rejects_invalid_or_unbounded_framing_before_body() {
    using WifiMaintenanceHttpPreflight::Decision;
    const char missingLength[] =
        "POST /api/device/settings HTTP/1.1\r\n"
        "X-V1Simple-Request: maintenance-ui\r\nContent-Type: application/json\r\n\r\n";
    assertPreflightDecision(missingLength, true, Decision::RejectLengthRequired);

    const char invalidLength[] =
        "POST /api/device/settings HTTP/1.1\r\n"
        "X-V1Simple-Request: maintenance-ui\r\nContent-Length: 2x\r\n\r\n";
    assertPreflightDecision(invalidLength, true, Decision::RejectBadRequest);

    const char duplicateLength[] =
        "POST /api/device/settings HTTP/1.1\r\n"
        "X-V1Simple-Request: maintenance-ui\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n";
    assertPreflightDecision(duplicateLength, true, Decision::RejectBadRequest);

    const char oversize[] =
        "POST /api/settings/restore HTTP/1.1\r\n"
        "X-V1Simple-Request: maintenance-ui\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 131073\r\n\r\n";
    assertPreflightDecision(oversize, true, Decision::RejectTooLarge);

    const char transferEncoding[] =
        "POST /api/settings/restore HTTP/1.1\r\n"
        "X-V1Simple-Request: maintenance-ui\r\n"
        "Content-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n";
    assertPreflightDecision(transferEncoding, true, Decision::RejectBadRequest);

    char oversizedHeaders[WifiMaintenanceHttpPreflight::kMaxHeaderBytes];
    for (size_t i = 0; i < sizeof(oversizedHeaders); ++i) {
        oversizedHeaders[i] = 'x';
    }
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Decision::RejectHeadersTooLarge),
        static_cast<int>(WifiMaintenanceHttpPreflight::evaluate(
            oversizedHeaders, sizeof(oversizedHeaders), true)));
}

void test_http_preflight_allows_supported_write_bodies_and_read_passthrough() {
    using WifiMaintenanceHttpPreflight::Decision;
    const char json[] =
        "POST /api/device/settings HTTP/1.1\r\n"
        "X-V1Simple-Request: maintenance-ui\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n\r\n{}";
    assertPreflightDecision(json, true, Decision::AllowBodyParsing);

    const char urlEncoded[] =
        "POST /api/device/settings HTTP/1.1\r\n"
        "X-V1Simple-Request: maintenance-ui\r\n"
        "Content-Type: application/x-www-form-urlencoded;charset=UTF-8\r\n"
        "Content-Length: 3\r\n\r\na=b";
    assertPreflightDecision(urlEncoded, true, Decision::AllowBodyParsing);

    const char bodyless[] =
        "POST /api/wifi/scan HTTP/1.1\r\n"
        "X-V1Simple-Request: maintenance-ui\r\n"
        "Content-Type: application/x-www-form-urlencoded;charset=UTF-8\r\n"
        "Content-Length: 0\r\n\r\n";
    assertPreflightDecision(bodyless, true, Decision::AllowBodyParsing);

    const char put[] =
        "PUT /api/device/settings HTTP/1.1\r\nX-V1Simple-Request: maintenance-ui\r\nContent-Length: 0\r\n\r\n";
    const char patch[] =
        "PATCH /api/device/settings HTTP/1.1\r\nX-V1Simple-Request: maintenance-ui\r\nContent-Length: 0\r\n\r\n";
    const char deleteRequest[] =
        "DELETE /api/device/settings HTTP/1.1\r\nX-V1Simple-Request: maintenance-ui\r\nContent-Length: 0\r\n\r\n";
    assertPreflightDecision(put, true, Decision::AllowBodyParsing);
    assertPreflightDecision(patch, true, Decision::AllowBodyParsing);
    assertPreflightDecision(deleteRequest, true, Decision::AllowBodyParsing);

    const char get[] = "GET /api/device/settings HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n";
    assertPreflightDecision(get, false, Decision::AllowFrameworkParsing);
}

void test_http_preflight_rate_admission_vetoes_only_valid_writes() {
    using WifiMaintenanceHttpPreflight::Decision;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Decision::RejectRateLimited),
        static_cast<int>(WifiMaintenanceHttpPreflight::applyWriteAdmission(Decision::AllowBodyParsing, false)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Decision::AllowBodyParsing),
        static_cast<int>(WifiMaintenanceHttpPreflight::applyWriteAdmission(Decision::AllowBodyParsing, true)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Decision::RejectForbidden),
                          static_cast<int>(WifiMaintenanceHttpPreflight::applyWriteAdmission(Decision::RejectForbidden, false)));
}

void test_failed_storage_resolution_clears_pre_admission_and_dispatches_one_503() {
    WebServer server(80);
    bool preAdmitted = true;
    int resolveCalls = 0;
    int handlerCalls = 0;

    WifiMaintenanceWritePolicy::dispatchStorageResolved(
        server, preAdmitted,
        [&]() {
            ++resolveCalls;
            return false;
        },
        [&]() { ++handlerCalls; });

    TEST_ASSERT_EQUAL_INT(1, resolveCalls);
    TEST_ASSERT_FALSE(preAdmitted);
    TEST_ASSERT_EQUAL_INT(0, handlerCalls);
    TEST_ASSERT_EQUAL_INT(1, server.sendCount);
    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("application/json", server.lastContentType.c_str());
    TEST_ASSERT_EQUAL_STRING(
        "{\"success\":false,\"error\":\"storage_transaction_recovery_pending\","
        "\"message\":\"Storage recovery is incomplete; retry this request\"}",
        server.lastBody.c_str());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_service_active_never_attempts);
    RUN_TEST(test_outside_maintenance_never_attempts);
    RUN_TEST(test_first_attempt_waits_for_first_retry_delay);
    RUN_TEST(test_repeat_attempts_follow_retry_interval);
    RUN_TEST(test_service_recovery_resets_schedule);
    RUN_TEST(test_now_zero_anchor_is_preserved_across_repeated_ticks);
    RUN_TEST(test_rollover_safe_delta);
    RUN_TEST(test_ap_bringup_abort_clears_stale_interface_state_for_all_consumers);
    RUN_TEST(test_maintenance_with_saved_credentials_uses_ap_sta_auto_connect);
    RUN_TEST(test_normal_setup_preserves_saved_sta_connectivity);
    RUN_TEST(test_maintenance_auto_connect_retries_failed_storage_resolution_then_starts);
    RUN_TEST(test_maintenance_http_interface_admission_allows_only_initialized_ap_destination);
    RUN_TEST(test_physical_auto_reconnect_is_reconciled_into_app_state);
    RUN_TEST(test_true_link_loss_waits_then_restarts_candidate_scan);
    RUN_TEST(test_explicit_disconnect_zero_deadline_never_auto_rejoins);
    RUN_TEST(test_explicit_disconnect_rejects_a_late_physical_connection);
    RUN_TEST(test_equal_ip_collision_forces_physical_sta_disconnect);
    RUN_TEST(test_link_retry_deadline_is_rollover_safe);
    RUN_TEST(test_failed_saved_network_persistence_has_no_runtime_side_effects);
    RUN_TEST(test_committed_mutation_cancels_old_auto_activity_and_only_affected_link);
    RUN_TEST(test_scan_mutation_restarts_but_unrelated_connecting_mutation_is_preserved);
    RUN_TEST(test_http_preflight_allows_bounded_legacy_multipart_posts);
    RUN_TEST(test_http_preflight_rejects_multipart_before_framework_parser);
    RUN_TEST(test_http_preflight_rejects_wrong_shape_and_nonmaintenance_before_body);
    RUN_TEST(test_http_preflight_rejects_invalid_or_unbounded_framing_before_body);
    RUN_TEST(test_http_preflight_allows_supported_write_bodies_and_read_passthrough);
    RUN_TEST(test_http_preflight_rate_admission_vetoes_only_valid_writes);
    RUN_TEST(test_failed_storage_resolution_clears_pre_admission_and_dispatches_one_503);
    return UNITY_END();
}
