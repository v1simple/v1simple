#!/usr/bin/env python3
"""Regression contract for project-owned maintenance write ingress."""

from pathlib import Path
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
        self.assertEqual(len(direct_post_registrations), 1)
        registrations = routes.count("registerMaintenanceWriteRoute(") - 1  # exclude method definition
        handler_checks = routes.count("requireMaintenanceWriteRequestShape()") - 1  # exclude method definition
        self.assertGreater(registrations, 0)
        self.assertEqual(registrations, handler_checks)
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
