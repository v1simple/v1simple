#!/usr/bin/env python3
"""Regression contract for project-owned maintenance write ingress."""

from pathlib import Path
import ast
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
ROUTES = ROOT / "src" / "wifi_routes.cpp"
MANAGER = ROOT / "src" / "wifi_manager.h"
FRONTEND_SRC = ROOT / "interface" / "src"


class MaintenanceIngressContractTest(unittest.TestCase):
    def test_all_post_routes_use_the_guarded_registration_seam(self) -> None:
        routes = ROUTES.read_text(encoding="utf-8")
        direct_post_registrations = re.findall(r"server_\.on\([^;]*HTTP_POST", routes, re.DOTALL)
        # One registration lives in the ordinary guarded wrapper and one in
        # the exact-body guarded wrapper. Product routes call those wrappers.
        self.assertEqual(len(direct_post_registrations), 2)
        registrations = routes.count("registerMaintenanceWriteRoute(") - 1  # exclude method definition
        exact_registrations = routes.count("registerMaintenanceExactBodyWriteRoute(") - 1
        handler_checks = routes.count("requireMaintenanceWriteRequestShape()") - 1  # exclude method definition
        self.assertGreater(registrations, 0)
        self.assertEqual(registrations + exact_registrations, handler_checks)
        wrapper_start = routes.index("void WiFiManager::registerMaintenanceWriteRoute")
        wrapper_end = routes.index("bool WiFiManager::setupWebServer", wrapper_start)
        wrapper = routes[wrapper_start:wrapper_end]
        self.assertIn("WifiMaintenanceWritePolicy::dispatchStorageResolved", wrapper)
        self.assertIn("settings_.resolveStorageTransactionsForMutation()", wrapper)

        policy = (ROOT / "src" / "modules" / "wifi" / "wifi_maintenance_write_policy.h").read_text(
            encoding="utf-8"
        )
        dispatch = policy[policy.index("dispatchStorageResolved") :]
        self.assertLess(dispatch.index("handler();"), dispatch.index("preAdmitted = false"))
        self.assertIn("server.send(503", dispatch)
        self.assertIn("storage_transaction_recovery_pending", dispatch)
        self.assertNotIn("WifiMaintenanceBodyIngress", routes)

        wifi_dir = ROOT / "src" / "modules" / "wifi"
        self.assertFalse((wifi_dir / "wifi_maintenance_body_ingress.h").exists())
        self.assertFalse((wifi_dir / "wifi_maintenance_body_policy.h").exists())

    def test_manager_uses_preflight_server_at_the_actual_socket_ingress(self) -> None:
        manager = MANAGER.read_text(encoding="utf-8")
        self.assertIn("WifiMaintenanceWebServer server_;", manager)
        ingress = (ROOT / "src" / "modules" / "wifi" / "wifi_maintenance_web_server.h").read_text(
            encoding="utf-8"
        )
        interface_check = ingress.index("WifiMaintenanceInterfacePolicy::allows")
        self.assertLess(interface_check, ingress.index("_currentStatus = HC_WAIT_READ"))
        self.assertLess(interface_check, ingress.index("inspectCurrentRequest()"))
        self.assertLess(interface_check, ingress.index("_parseRequest(_currentClient)"))
        self.assertIn("_currentClient.localIP()", ingress)
        self.assertIn("liveStaIp_()", ingress)
        self.assertIn("_currentClient.stop()", ingress[interface_check : ingress.index("_currentStatus = HC_WAIT_READ")])
        self.assertLess(ingress.index("inspectCurrentRequest()"), ingress.index("_parseRequest(_currentClient)"))
        inspect_definition = ingress.index("Decision inspectCurrentRequest()")
        inspect_body = ingress[inspect_definition : ingress.index("void sendPreflightError", inspect_definition)]
        admission_policy = inspect_body.index("applyWriteAdmission")
        admission = inspect_body.index("writeAdmission_()")
        self.assertIn("Decision::AllowBodyParsing", inspect_body[:admission])
        self.assertLess(admission_policy, admission)
        self.assertIn("MSG_PEEK", ingress)

        routes = ROUTES.read_text(encoding="utf-8")
        self.assertIn("setLiveStaIp", routes)
        self.assertIn("WiFi.localIP()", routes)
        self.assertIn("WiFi.status() == WL_CONNECTED", routes)

    def test_large_json_routes_use_one_psram_body_and_release_it_at_every_terminal_path(self) -> None:
        ingress = (ROOT / "src" / "modules" / "wifi" / "wifi_maintenance_web_server.h").read_text(
            encoding="utf-8"
        )
        routes = ROUTES.read_text(encoding="utf-8")
        self.assertIn("MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM", ingress)
        self.assertIn("void releaseExactBody()", ingress)
        self.assertIn("if (raw.status == RAW_ABORTED)", ingress)
        self.assertIn("clearRequestIngress();", ingress[ingress.index("if (!keepCurrentClient)") :])
        self.assertIn("preflightBodyInfo_.contentLength", ingress)
        self.assertIn("WifiExactBodyLengthPolicy::begin", ingress)
        self.assertIn("WifiExactBodyLengthPolicy::acceptsEnd", ingress)
        self.assertIn("kRawLengthContractSignal = 0x56314232u", ingress)
        exact_route = routes[routes.index("void WiFiManager::registerMaintenanceExactBodyWriteRoute") :
                             routes.index("bool WiFiManager::setupWebServer")]
        self.assertGreaterEqual(exact_route.count("server_.releaseExactBody();"), 2)
        self.assertLess(exact_route.index("handler(body, bodySize, multipartBoundary, multipartBoundarySize)"),
                        exact_route.rindex("server_.releaseExactBody();"))
        for path in ('"/api/v1/profile"', '"/api/v1/profile/delete"', '"/api/settings/restore"'):
            self.assertIn(f"registerMaintenanceExactBodyWriteRoute({path}", routes)

        patch = (ROOT / "scripts" / "patch_arduino_webserver_body.py").read_text(encoding="utf-8")
        self.assertIn("RAW_PATCHED", patch)
        self.assertIn("rawExpectedLength", patch)
        self.assertIn("(uintptr_t)_currentRaw->data == 0x56314232u", patch)
        self.assertIn("if (_currentHandler && _currentHandler->canRaw", patch)
        module = ast.parse(patch)
        string_patched = next(
            ast.literal_eval(node.value)
            for node in module.body
            if isinstance(node, ast.Assign)
            and isinstance(node.targets[0], ast.Name)
            and node.targets[0].id == "STRING_PATCHED"
        )
        self.assertEqual(len(string_patched.splitlines()), 2)
        self.assertTrue(string_patched.splitlines()[1].lstrip().startswith("arg.value ="))

    def test_all_formdata_callers_flow_through_the_shared_fetch_wrapper(self) -> None:
        callers: list[str] = []
        direct_fetches: list[str] = []
        for path in FRONTEND_SRC.rglob("*"):
            if path.suffix not in {".js", ".svelte"} or ".test." in path.name:
                continue
            source = path.read_text(encoding="utf-8")
            relative = str(path.relative_to(ROOT))
            if "new FormData(" in source:
                callers.append(relative)
                self.assertTrue(
                    "fetchWithTimeout" in source or "postSettingsForm" in source,
                    f"FormData caller bypasses the normalized request wrapper: {relative}",
                )
            if path.name != "poll.js" and re.search(r"\bfetch\s*\(", source):
                direct_fetches.append(relative)

        self.assertGreater(len(callers), 0)
        self.assertEqual(direct_fetches, [])


if __name__ == "__main__":
    unittest.main()
