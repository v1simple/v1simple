#!/usr/bin/env python3
"""Regression tests for the WiFi API maintenance/runtime contract."""

from __future__ import annotations

import importlib.util
import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch


SCRIPT_PATH = Path(__file__).with_name("check_wifi_api_contract.py")
SPEC = importlib.util.spec_from_file_location("check_wifi_api_contract", SCRIPT_PATH)
assert SPEC is not None and SPEC.loader is not None
CONTRACT = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CONTRACT
SPEC.loader.exec_module(CONTRACT)


SOURCE = """
void WiFiManager::setupWebServer() {
    server_.on("/api/live", HTTP_GET, [this]() {
        AlpApiService::handleApiStatus();
    });
    server_.on("/api/offline", HTTP_POST, [this]() {
        WifiSettingsApiService::handleApiDeviceSettingsSave();
    });
    server_.on("/ping", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiPing();
    });
}
"""

COMPLETE_POLICY = [
    "route=HTTP_GET /api/live runtime=alp "
    "normal_absent=runtime_unavailable normal_present=serve "
    "maintenance_absent=maintenance_conflict maintenance_present=maintenance_conflict "
    "delegates=AlpApiService::handleApiStatus",
    "route=HTTP_POST /api/offline runtime=none "
    "normal_absent=serve normal_present=serve "
    "maintenance_absent=serve maintenance_present=serve "
    "delegates=WifiSettingsApiService::handleApiDeviceSettingsSave",
]

COMPOSITION_SOURCE = """
void WiFiManager::setupWebServer() {
    server_.on("/api/alp/status", HTTP_GET, [this]() {
        AlpApiService::handleApiStatus(
            server_, alpRuntime_, nullptr, this,
            mainRuntimeState.maintenanceBootActive);
    });
    server_.on("/api/gps/config", HTTP_POST, [this]() {
        GpsApiService::Runtime r;
        r.maintenanceBootActive = mainRuntimeState.maintenanceBootActive;
        GpsApiService::handleApiConfigSave(server_, settingsManager, gpsRuntime_, r);
    });
    server_.on("/api/gps/status", HTTP_GET, [this]() {
        GpsApiService::Runtime r;
        r.maintenanceBootActive = mainRuntimeState.maintenanceBootActive;
        GpsApiService::handleApiStatus(server_, gpsRuntime_, r);
    });
    server_.on("/api/obd/status", HTTP_GET, [this]() {
        ObdApiService::handleApiStatus(server_, obdRuntime_, makeObdRuntime());
    });
    server_.on("/api/obd/devices", HTTP_GET, [this]() {
        ObdApiService::handleApiDevicesList(
            server_, obdRuntime_, settingsManager, makeObdRuntime());
    });
    server_.on("/api/obd/scan", HTTP_POST, [this]() {
        ObdApiService::handleApiScan(server_, obdRuntime_, makeObdRuntime());
    });
    server_.on("/api/obd/forget", HTTP_POST, [this]() {
        ObdApiService::handleApiForget(
            server_, obdRuntime_, settingsManager, makeObdRuntime());
    });
    server_.on("/api/obd/config", HTTP_POST, [this]() {
        ObdApiService::handleApiConfig(
            server_, obdRuntime_, settingsManager, makeObdRuntime());
    });
}

ObdApiService::Runtime WiFiManager::makeObdRuntime() {
    ObdApiService::Runtime r;
    r.maintenanceBootActive = mainRuntimeState.maintenanceBootActive;
    return r;
}
"""

ENABLE_TRANSACTION_SOURCE = (
    "bool WiFiManager::enableWifiClientFromSavedCredentials() {\n"
    f"{CONTRACT.WIFI_ENABLE_TRANSACTION_BODY_CONTRACT}\n"
    "}\n"
)


class MaintenanceRuntimePolicyContractTest(unittest.TestCase):
    def test_complete_policy_covers_each_api_service_route(self) -> None:
        self.assertEqual(
            CONTRACT.find_maintenance_runtime_policy_errors(SOURCE, COMPLETE_POLICY),
            [],
        )

    def test_repository_contract_has_all_61_routes(self) -> None:
        lines = CONTRACT.read_expected_lines(
            CONTRACT.MAINTENANCE_RUNTIME_POLICY_CONTRACT_FILE
        )
        policies, errors = CONTRACT.parse_maintenance_runtime_policy(lines)
        self.assertEqual(errors, [])
        self.assertEqual(len(policies), 61)

    def test_missing_policy_row_fails(self) -> None:
        errors = CONTRACT.find_maintenance_runtime_policy_errors(
            SOURCE, COMPLETE_POLICY[:1]
        )
        self.assertIn("missing policy row: HTTP_POST /api/offline", errors)

    def test_extra_policy_row_fails(self) -> None:
        rows = COMPLETE_POLICY + [
            "route=HTTP_GET /api/removed runtime=none "
            "normal_absent=serve normal_present=serve "
            "maintenance_absent=serve maintenance_present=serve "
            "delegates=RemovedApiService::handleApiRemoved"
        ]
        errors = CONTRACT.find_maintenance_runtime_policy_errors(SOURCE, rows)
        self.assertIn("extra policy row: HTTP_GET /api/removed", errors)

    def test_duplicate_policy_row_fails(self) -> None:
        rows = COMPLETE_POLICY + [COMPLETE_POLICY[0]]
        errors = CONTRACT.find_maintenance_runtime_policy_errors(SOURCE, rows)
        self.assertIn("duplicate policy row: HTTP_GET /api/live", errors)

    def test_duplicate_api_service_route_registration_fails(self) -> None:
        source = SOURCE + """
server_.on("/api/live", HTTP_GET, [this]() {
    AlpApiService::handleApiStatus();
});
"""
        errors = CONTRACT.find_maintenance_runtime_policy_errors(
            source, COMPLETE_POLICY
        )
        self.assertIn(
            "duplicate ApiService route registration: HTTP_GET /api/live x2", errors
        )

    def test_delegate_drift_fails(self) -> None:
        rows = [
            COMPLETE_POLICY[0].replace(
                "AlpApiService::handleApiStatus", "AlpApiService::handleApiOther"
            ),
            COMPLETE_POLICY[1],
        ]
        errors = CONTRACT.find_maintenance_runtime_policy_errors(SOURCE, rows)
        self.assertTrue(
            any(
                error.startswith("delegate mismatch for HTTP_GET /api/live")
                for error in errors
            )
        )

    def test_unknown_runtime_and_cell_expectation_fail_closed(self) -> None:
        rows = [
            COMPLETE_POLICY[0]
            .replace("runtime=alp", "runtime=missing")
            .replace("normal_absent=runtime_unavailable", "normal_absent=unknown"),
            COMPLETE_POLICY[1],
        ]
        errors = CONTRACT.find_maintenance_runtime_policy_errors(SOURCE, rows)
        self.assertIn("invalid runtime family for HTTP_GET /api/live: missing", errors)
        self.assertIn(
            "invalid normal_absent expectation for HTTP_GET /api/live: unknown",
            errors,
        )

    def test_unsupported_cell_combination_fails_closed(self) -> None:
        rows = [
            COMPLETE_POLICY[0].replace(
                "maintenance_present=maintenance_conflict",
                "maintenance_present=serve",
            ),
            COMPLETE_POLICY[1],
        ]
        errors = CONTRACT.find_maintenance_runtime_policy_errors(SOURCE, rows)
        self.assertTrue(
            any(error.startswith("unsupported four-cell matrix") for error in errors)
        )

    def test_runtime_none_cannot_vary_by_runtime_presence(self) -> None:
        rows = [
            COMPLETE_POLICY[0],
            COMPLETE_POLICY[1]
            .replace("normal_absent=serve", "normal_absent=runtime_unavailable"),
        ]
        errors = CONTRACT.find_maintenance_runtime_policy_errors(SOURCE, rows)
        self.assertIn(
            "runtime=none policy varies by runtime presence for "
            "HTTP_POST /api/offline",
            errors,
        )

    def test_missing_cell_is_malformed_and_does_not_cover_route(self) -> None:
        rows = [
            COMPLETE_POLICY[0].replace(" normal_present=serve", ""),
            COMPLETE_POLICY[1],
        ]
        errors = CONTRACT.find_maintenance_runtime_policy_errors(SOURCE, rows)
        self.assertTrue(any(error.startswith("malformed policy row 1") for error in errors))
        self.assertIn("missing policy row: HTTP_GET /api/live", errors)


class NativeDelegateCoverageContractTest(unittest.TestCase):
    @staticmethod
    def policies() -> dict[str, CONTRACT.MaintenanceRuntimePolicy]:
        policies, errors = CONTRACT.parse_maintenance_runtime_policy(COMPLETE_POLICY)
        if errors:
            raise AssertionError(errors)
        return policies

    def test_qualified_invocations_cover_declared_delegates(self) -> None:
        sources = {
            "test/test_example_api_service/test_example.cpp": """
void test_handlers() {
    AlpApiService::handleApiStatus();
    WifiSettingsApiService::handleApiDeviceSettingsSave();
}
"""
        }
        self.assertEqual(
            CONTRACT.find_native_delegate_coverage_errors(self.policies(), sources),
            [],
        )

    def test_missing_qualified_invocation_fails(self) -> None:
        sources = {
            "test/test_example_api_service/test_example.cpp": """
void test_handler() {
    AlpApiService::handleApiStatus();
}
"""
        }
        self.assertIn(
            "missing qualified native ApiService invocation: "
            "WifiSettingsApiService::handleApiDeviceSettingsSave",
            CONTRACT.find_native_delegate_coverage_errors(self.policies(), sources),
        )

    def test_definitions_and_preprocessor_directives_do_not_count(self) -> None:
        sources = {
            "test/test_example_api_service/test_example.cpp": r"""
#include "../../src/AlpApiService::handleApiStatus.cpp"
#define FAKE_COVERAGE() WifiSettingsApiService::handleApiDeviceSettingsSave()

void AlpApiService::handleApiStatus(
    WebServer& server) {
    (void)server;
}
"""
        }
        errors = CONTRACT.find_native_delegate_coverage_errors(
            self.policies(), sources
        )
        self.assertEqual(len(errors), 2)
        self.assertTrue(all("missing qualified native" in error for error in errors))

    def test_unqualified_handler_call_does_not_count(self) -> None:
        sources = {
            "test/test_example_api_service/test_example.cpp": """
void test_handler() {
    handleApiStatus();
    WifiSettingsApiService::handleApiDeviceSettingsSave();
}
"""
        }
        self.assertIn(
            "missing qualified native ApiService invocation: "
            "AlpApiService::handleApiStatus",
            CONTRACT.find_native_delegate_coverage_errors(self.policies(), sources),
        )

    def test_repository_has_qualified_invocations_for_all_61_delegates(self) -> None:
        lines = CONTRACT.read_expected_lines(
            CONTRACT.MAINTENANCE_RUNTIME_POLICY_CONTRACT_FILE
        )
        policies, errors = CONTRACT.parse_maintenance_runtime_policy(lines)
        self.assertEqual(errors, [])
        delegates = {
            delegate for policy in policies.values() for delegate in policy.delegates
        }
        self.assertEqual(len(delegates), 61)
        self.assertEqual(
            CONTRACT.find_native_delegate_coverage_errors(
                policies, CONTRACT.read_native_api_service_test_sources()
            ),
            [],
        )

    def test_update_refuses_to_bypass_missing_native_delegate_coverage(self) -> None:
        output = io.StringIO()
        with (
            patch.object(CONTRACT, "read_native_api_service_test_sources", return_value={}),
            patch.object(sys, "argv", [str(SCRIPT_PATH), "--update"]),
            redirect_stdout(output),
        ):
            self.assertEqual(CONTRACT.main(), 1)
        self.assertIn("[contract] native-delegate-coverage mismatch", output.getvalue())
        self.assertIn(
            "missing qualified native ApiService invocation",
            output.getvalue(),
        )


class NullableRuntimeRouteCompositionTest(unittest.TestCase):
    def test_nullable_runtime_and_maintenance_composition_matches(self) -> None:
        self.assertEqual(
            CONTRACT.find_nullable_runtime_route_composition_errors(
                COMPOSITION_SOURCE
            ),
            [],
        )

    def test_pointer_dereference_before_service_fails(self) -> None:
        source = COMPOSITION_SOURCE.replace(
            "server_, alpRuntime_, nullptr", "server_, *alpRuntime_, nullptr", 1
        )
        errors = CONTRACT.find_nullable_runtime_route_composition_errors(source)
        self.assertIn(
            "runtime dereferenced before service for HTTP_GET /api/alp/status: "
            "alpRuntime_",
            errors,
        )

    def test_alp_maintenance_state_must_be_a_direct_service_argument(self) -> None:
        source = COMPOSITION_SOURCE.replace(
            "mainRuntimeState.maintenanceBootActive);",
            "false);",
            1,
        )
        errors = CONTRACT.find_nullable_runtime_route_composition_errors(source)
        self.assertIn(
            "maintenance state not passed to service for HTTP_GET /api/alp/status",
            errors,
        )

    def test_route_level_null_rejection_before_service_fails(self) -> None:
        source = COMPOSITION_SOURCE.replace(
            "GpsApiService::Runtime r;",
            "if (!gpsRuntime_) return;\n        GpsApiService::Runtime r;",
            1,
        )
        errors = CONTRACT.find_nullable_runtime_route_composition_errors(source)
        self.assertIn(
            "runtime accessed before service for HTTP_POST /api/gps/config: "
            "gpsRuntime_",
            errors,
        )

    def test_gps_maintenance_assignment_must_precede_service(self) -> None:
        source = COMPOSITION_SOURCE.replace(
            "r.maintenanceBootActive = mainRuntimeState.maintenanceBootActive;\n"
            "        GpsApiService::handleApiConfigSave",
            "GpsApiService::handleApiConfigSave",
            1,
        )
        errors = CONTRACT.find_nullable_runtime_route_composition_errors(source)
        self.assertIn(
            "maintenance state not populated before service for "
            "HTTP_POST /api/gps/config",
            errors,
        )

    def test_obd_factory_must_propagate_maintenance_state(self) -> None:
        source = COMPOSITION_SOURCE.replace(
            "r.maintenanceBootActive = mainRuntimeState.maintenanceBootActive;\n"
            "    return r;",
            "return r;",
            1,
        )
        errors = CONTRACT.find_nullable_runtime_route_composition_errors(source)
        self.assertIn(
            "makeObdRuntime() must propagate maintenance state before return", errors
        )


class WifiEnableTransactionContractTest(unittest.TestCase):
    def test_transactional_manager_wiring_matches(self) -> None:
        self.assertEqual(
            CONTRACT.find_wifi_enable_transaction_errors(ENABLE_TRANSACTION_SOURCE),
            [],
        )

    def test_prestart_persistence_fails(self) -> None:
        source = ENABLE_TRANSACTION_SOURCE.replace(
            "WifiClientEnableTransaction::Runtime runtime;",
            "settingsManager.setWifiClientEnabled(true);\n"
            "    WifiClientEnableTransaction::Runtime runtime;",
        )
        errors = CONTRACT.find_wifi_enable_transaction_errors(source)
        self.assertIn(
            "wifi enable transaction must contain exactly one enabled persistence commit",
            errors,
        )

    def test_automatic_maintenance_gate_cannot_replace_explicit_intent(self) -> None:
        source = ENABLE_TRANSACTION_SOURCE.replace(
            "beginMaintenanceAutoConnectScan(true)",
            "beginMaintenanceAutoConnectScan(false)",
        )
        errors = CONTRACT.find_wifi_enable_transaction_errors(source)
        self.assertIn(
            "wifi enable transaction missing explicit maintenance enable admission",
            errors,
        )

    def test_manager_cannot_bypass_transaction_execution(self) -> None:
        source = ENABLE_TRANSACTION_SOURCE.replace(
            "return WifiClientEnableTransaction::execute(runtime);",
            "return true;",
        )
        errors = CONTRACT.find_wifi_enable_transaction_errors(source)
        self.assertIn("wifi enable transaction missing transaction execution", errors)

    def test_persistence_cannot_move_outside_commit_lambda(self) -> None:
        source = ENABLE_TRANSACTION_SOURCE.replace(
            "runtime.commitEnabled = [](void*) { "
            "settingsManager.setWifiClientEnabled(true); };",
            "runtime.commitEnabled = [](void*) {};\n"
            "    settingsManager.setWifiClientEnabled(true);",
        )
        errors = CONTRACT.find_wifi_enable_transaction_errors(source)
        self.assertIn(
            "wifi enable persistence must remain inside the commit callback", errors
        )

    def test_rollback_lambda_cannot_drop_prior_runtime_restoration(self) -> None:
        source = ENABLE_TRANSACTION_SOURCE.replace(
            "runtime.rollbackFailedStart = [](void* ctx) {\n"
            "        auto* transaction = static_cast<EnableContext*>(ctx);\n"
            "        transaction->manager->wifiClientState_ = transaction->priorState;\n"
            "        transaction->manager->currentConnectedSlotIndex_ = "
            "transaction->priorConnectedSlotIndex;\n"
            "    };",
            "runtime.rollbackFailedStart = [](void*) {};",
        )
        errors = CONTRACT.find_wifi_enable_transaction_errors(source)
        self.assertIn(
            "wifi enable rollback missing prior client state restoration", errors
        )
        self.assertIn(
            "wifi enable rollback missing prior connected slot restoration", errors
        )

    def test_lifecycle_idempotence_must_include_maintenance_connecting(self) -> None:
        source = ENABLE_TRANSACTION_SOURCE.replace(
            "maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::CONNECTING",
            "false",
        )
        errors = CONTRACT.find_wifi_enable_transaction_errors(source)
        self.assertIn(
            "wifi enable transaction missing lifecycle admission snapshot", errors
        )

    def test_lifecycle_idempotence_cannot_change_or_to_and(self) -> None:
        source = ENABLE_TRANSACTION_SOURCE.replace(
            "wifiClientState_ == WIFI_CLIENT_CONNECTING ||",
            "wifiClientState_ == WIFI_CLIENT_CONNECTING &&",
        )
        errors = CONTRACT.find_wifi_enable_transaction_errors(source)
        self.assertIn(
            "wifi enable transaction missing lifecycle admission snapshot", errors
        )

    def test_transaction_snapshots_cannot_be_overwritten(self) -> None:
        source = ENABLE_TRANSACTION_SOURCE.replace(
            "runtime.attemptStart = [](void* ctx) {",
            "runtime.persistedEnabled = false;\n"
            "    runtime.lifecycleAdmitted = false;\n"
            "    runtime.attemptStart = [](void* ctx) {",
        )
        errors = CONTRACT.find_wifi_enable_transaction_errors(source)
        self.assertIn("wifi enable transaction missing persisted enable snapshot", errors)
        self.assertIn(
            "wifi enable transaction missing lifecycle admission snapshot", errors
        )

    def test_commit_cannot_be_made_conditional(self) -> None:
        source = ENABLE_TRANSACTION_SOURCE.replace(
            "settingsManager.setWifiClientEnabled(true);",
            "if (false) { settingsManager.setWifiClientEnabled(true); }",
        )
        errors = CONTRACT.find_wifi_enable_transaction_errors(source)
        self.assertIn(
            "wifi enable persistence must remain inside the commit callback", errors
        )

    def test_rollback_cannot_be_made_conditional(self) -> None:
        source = ENABLE_TRANSACTION_SOURCE.replace(
            "transaction->manager->wifiClientState_ = transaction->priorState;",
            "if (false) {\n"
            "            transaction->manager->wifiClientState_ = transaction->priorState;\n"
            "        }",
        )
        errors = CONTRACT.find_wifi_enable_transaction_errors(source)
        self.assertIn(
            "wifi enable rollback must remain an unconditional prior-state restoration",
            errors,
        )

    def test_transaction_context_cannot_be_removed(self) -> None:
        source = ENABLE_TRANSACTION_SOURCE.replace("    runtime.ctx = &transaction;\n", "")
        errors = CONTRACT.find_wifi_enable_transaction_errors(source)
        self.assertIn(
            "wifi enable transaction missing canonical transaction context", errors
        )

    def test_callbacks_cannot_be_overwritten(self) -> None:
        replacements = {
            "start": "runtime.attemptStart = [](void*) { return true; };",
            "rollback": "runtime.rollbackFailedStart = [](void*) {};",
            "commit": "runtime.commitEnabled = [](void*) {};",
        }
        for label, overwrite in replacements.items():
            with self.subTest(label=label):
                source = ENABLE_TRANSACTION_SOURCE.replace(
                    "    return WifiClientEnableTransaction::execute(runtime);",
                    f"    {overwrite}\n"
                    "    return WifiClientEnableTransaction::execute(runtime);",
                )
                errors = CONTRACT.find_wifi_enable_transaction_errors(source)
                self.assertIn(
                    f"wifi enable transaction {label} callback must be assigned exactly once",
                    errors,
                )

    def test_explicit_maintenance_admission_cannot_be_guarded_false(self) -> None:
        source = ENABLE_TRANSACTION_SOURCE.replace(
            "if (self->beginMaintenanceAutoConnectScan(true))",
            "if (false && self->beginMaintenanceAutoConnectScan(true))",
        )
        errors = CONTRACT.find_wifi_enable_transaction_errors(source)
        self.assertIn(
            "wifi enable start callback differs from reviewed admission flow", errors
        )

    def test_transaction_cannot_execute_twice(self) -> None:
        source = ENABLE_TRANSACTION_SOURCE.replace(
            "return WifiClientEnableTransaction::execute(runtime);",
            "(void)WifiClientEnableTransaction::execute(runtime);\n"
            "    return WifiClientEnableTransaction::execute(runtime);",
        )
        errors = CONTRACT.find_wifi_enable_transaction_errors(source)
        self.assertIn("wifi enable transaction must execute exactly once", errors)


if __name__ == "__main__":
    unittest.main()
