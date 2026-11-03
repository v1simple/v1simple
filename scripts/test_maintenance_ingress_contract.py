#!/usr/bin/env python3
"""Regression contract for project-owned maintenance write ingress."""

from pathlib import Path
import ast
import re
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
ROUTES = ROOT / "src" / "wifi_routes.cpp"
MANAGER = ROOT / "src" / "wifi_manager.h"
FRONTEND_SRC = ROOT / "interface" / "src"


def write_boundary_sources(root: Path, out: Path) -> None:
    """Compile the real registration API, wrapper bodies and callers with socket adapters.

    Domain payload parsing stays in its existing service suites. These fixtures
    do not substitute implementations of preflight or the project WebServer.
    """
    manager = (root / "src/wifi_manager.h").read_text(encoding="utf-8")
    routes = (root / "src/wifi_routes.cpp").read_text(encoding="utf-8")
    lifecycle = (root / "src/wifi_manager_lifecycle.cpp").read_text(encoding="utf-8")
    declaration_start = manager.index("    template <MaintenanceWriteRoutes::Id id, typename Handler>")
    declaration_end = manager.index("    static const char* maintenanceApiWriteHeader();", declaration_start)
    (out / "registration_declarations.inc").write_text(manager[declaration_start:declaration_end], encoding="utf-8")
    implementation_start = routes.index("const char* WiFiManager::maintenanceApiWriteHeader()")
    implementation_end = routes.index("bool WiFiManager::setupWebServer()", implementation_start)
    rate_start = lifecycle.index("bool WiFiManager::checkRateLimit()")
    rate_end = lifecycle.index("// Web activity tracking", rate_start)
    (out / "registration_implementations.inc").write_text(
        routes[implementation_start:implementation_end] + lifecycle[rate_start:rate_end], encoding="utf-8")
    setup = routes[implementation_end:]
    if re.search(r"server_\.on\([^;]*HTTP_POST", setup, re.DOTALL):
        raise AssertionError("maintenance POST bypasses its typed registration boundary")
    calls = []
    for match in re.finditer(
        r"(registerMaintenanceWriteRoute<MaintenanceWriteRoutes::Id::\w+>\()\s*\[this\]\s*\(([^)]*)\)", setup
    ):
        # Keep each actual registration ID and actual handler kind. The fixture
        # domain callback observes the wrapper boundary instead of mutating NVS.
        arguments = match.group(2)
        if "const uint8_t*" in arguments:
            handler = "[this](const uint8_t* body, size_t size, const char* boundary, size_t boundarySize) { service(body, size, boundary, boundarySize); }"
        elif not arguments.strip():
            handler = "[this]() { service(); }"
        else:
            raise AssertionError(f"unexpected maintenance handler signature: {arguments}")
        calls.append(match.group(1) + handler + ");")
    if not calls:
        raise AssertionError("no production maintenance registrations found")
    (out / "registration_calls.inc").write_text("\n".join(calls) + "\n", encoding="utf-8")


class MaintenanceIngressContractTest(unittest.TestCase):
    def test_actual_registration_and_socket_boundary(self) -> None:
        compiler = shlex.split(os.environ.get("CXX", shutil.which("clang++") or "c++"))
        fixture = ROOT / "test" / "fixtures" / "maintenance_ingress"
        with tempfile.TemporaryDirectory(prefix="maintenance-ingress-") as temporary:
            out = Path(temporary)
            write_boundary_sources(ROOT, out)
            flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-DUNIT_TEST",
                     "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g",
                     "-I" + str(out), "-I" + str(fixture), "-I" + str(ROOT / "test/mocks"),
                     "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src")]
            subprocess.run(compiler + flags + [str(fixture / "ingress.cpp"), "-o", str(out / "ingress")], check=True)
            subprocess.run([str(out / "ingress")], check=True)
            for route_id, handler, diagnostic in (
                ("V1Profile", "[this]() {}", "Exact maintenance route requires"),
                ("GpsConfig", "[this](const uint8_t*, size_t, const char*, size_t) {}", "Framework maintenance route requires"),
            ):
                (out / "registration_calls.inc").write_text(
                    f"registerMaintenanceWriteRoute<MaintenanceWriteRoutes::Id::{route_id}>({handler});\n",
                    encoding="utf-8",
                )
                result = subprocess.run(compiler + flags + ["-fsyntax-only", str(fixture / "ingress.cpp")],
                                        capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(diagnostic, result.stderr)

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
