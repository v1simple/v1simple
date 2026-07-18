#!/usr/bin/env python3
"""Check WiFi API route/policy contracts for extracted ApiService endpoints.

This script enforces WiFi API route/policy invariants across extracted WiFi route files:
1) Route contract for extracted API modules stays stable (method + path).
2) Route-lambda policy contract for ApiService endpoints stays stable
   (rate-limit, UI activity mark, delegate calls).
3) WiFiManager handle* methods do not become thin ApiService shims again.
4) ApiService delegates remain bound only in setupWebServer() route registration.
5) Remaining local WiFi route families preserve route-level policy and handler bindings.

Use --update to rewrite expected contract snapshots from current source.
"""

from __future__ import annotations

import argparse
from collections import Counter
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple

ROOT = Path(__file__).resolve().parents[1]
SRC_FILES = [
    ROOT / "src" / "wifi_routes.cpp",
    ROOT / "src" / "wifi_runtimes.cpp",
    ROOT / "src" / "wifi_client.cpp",
]
ROUTE_CONTRACT_FILE = ROOT / "test" / "contracts" / "wifi_route_contract.txt"
POLICY_CONTRACT_FILE = ROOT / "test" / "contracts" / "wifi_handler_policy_contract.txt"
SHIM_ABSENCE_CONTRACT_FILE = (
    ROOT / "test" / "contracts" / "wifi_shim_absence_contract.txt"
)
LOCAL_HANDLER_ROUTE_CONTRACT_FILE = (
    ROOT / "test" / "contracts" / "wifi_local_handler_route_contract.txt"
)
API_WRITE_GUARD_CALL = "requireMaintenanceApiWriteHeader()"

ROUTE_PREFIXES = (
    "/api/diagnostics/",
    "/api/settings/backup",
    "/api/settings/restore",
    "/api/system/",
)
POLICY_CALLBACK_PREFIXES: Tuple[str, ...] = ()
LOCAL_HANDLER_ROUTE_KEYS: Tuple[str, ...] = (
    "HTTP_GET /api/status",
    "HTTP_GET /api/device/settings",
    "HTTP_POST /api/device/settings",
    "HTTP_GET /api/v1/profiles",
    "HTTP_GET /api/v1/profile",
    "HTTP_POST /api/v1/profile",
    "HTTP_POST /api/v1/profile/delete",
    "HTTP_POST /api/v1/pull",
    "HTTP_POST /api/v1/push",
    "HTTP_GET /api/v1/current",
    "HTTP_GET /api/v1/devices",
    "HTTP_POST /api/v1/devices/name",
    "HTTP_POST /api/v1/devices/profile",
    "HTTP_POST /api/v1/devices/delete",
    "HTTP_GET /api/autopush/slots",
    "HTTP_POST /api/autopush/slot",
    "HTTP_POST /api/autopush/activate",
    "HTTP_POST /api/autopush/push",
    "HTTP_GET /api/autopush/status",
    "HTTP_GET /api/quiet/settings",
    "HTTP_GET /api/display/settings",
    "HTTP_GET /api/obd/config",
    "HTTP_GET /api/obd/devices",
    "HTTP_POST /api/quiet/settings",
    "HTTP_POST /api/display/settings",
    "HTTP_POST /api/display/settings/reset",
    "HTTP_POST /api/display/preview",
    "HTTP_POST /api/display/preview/clear",
    "HTTP_GET /api/display/visual/steps",
    "HTTP_GET /api/display/visual/layout",
    "HTTP_POST /api/display/visual/pin",
    "HTTP_GET /api/display/visual/framebuffer",
    "HTTP_GET /api/display/visual/flushshadow",
    "HTTP_POST /api/display/visual/clear",
    "HTTP_GET /api/obd/status",
    "HTTP_POST /api/obd/devices/name",
    "HTTP_POST /api/obd/scan",
    "HTTP_POST /api/obd/forget",
    "HTTP_POST /api/obd/config",
    "HTTP_GET /api/wifi/status",
    "HTTP_GET /api/wifi/scan",
    "HTTP_GET /api/wifi/networks",
    "HTTP_POST /api/wifi/scan",
    "HTTP_POST /api/wifi/disconnect",
    "HTTP_POST /api/wifi/forget",
    "HTTP_POST /api/wifi/enable",
    "HTTP_POST /api/wifi/networks",
    "HTTP_POST /api/wifi/networks/delete",
    "HTTP_POST /api/wifi/networks/test",
    "HTTP_GET /api/audio/settings",
    "HTTP_POST /api/audio/settings",
    "HTTP_GET /api/alp/status",
    "HTTP_GET /api/gps/config",
    "HTTP_POST /api/gps/config",
    "HTTP_GET /api/gps/status",
)

# NOTE: every literal delimiter below is followed by \s* so that clang-format is
# free to re-wrap a route registration across lines (breaking after `server_.on(`,
# after the path string, or before the lambda) without the route silently
# disappearing from the contract. A route that stops matching here would skip the
# POST write-guard check entirely, so these must stay whitespace-insensitive.
ROUTE_SIGNATURE_RE = re.compile(r'server_\.on\(\s*"([^"]+)"\s*,\s*(HTTP_[A-Z]+)\s*,')
ROUTE_LAMBDA_START_RE = re.compile(
    r'server_\.on\(\s*"([^"]+)"\s*,\s*(HTTP_[A-Z]+)\s*,\s*\[[^\]]*\]\s*\(\s*\)\s*\{'
)
HANDLE_METHOD_START_RE = re.compile(r"void\s+WiFiManager::(handle[A-Za-z0-9_]+)\s*\(\s*\)\s*\{")
METHOD_START_RE = re.compile(r"(?:void|bool)\s+WiFiManager::([A-Za-z0-9_]+)\s*\([^)]*\)\s*\{")
DELEGATE_RE = re.compile(r"([A-Za-z0-9_]+ApiService::[A-Za-z0-9_]+)\s*\(")
HANDLE_CALL_RE = re.compile(r"(?<!::)\b(handle[A-Za-z0-9_]+)\s*\(")


@dataclass(frozen=True)
class RoutePolicy:
    route: str
    rate_limit: int
    ui_activity: int
    delegates: Tuple[str, ...]

    def to_line(self) -> str:
        delegate_blob = ",".join(self.delegates)
        return (
            f"route={self.route} "
            f"rate_limit={self.rate_limit} "
            f"ui_activity={self.ui_activity} "
            f"delegates={delegate_blob}"
        )


@dataclass(frozen=True)
class LocalHandlerRoutePolicy:
    route: str
    rate_limit: int
    ui_activity: int
    handlers: Tuple[str, ...]
    delegates: Tuple[str, ...]

    def to_line(self) -> str:
        handler_blob = ",".join(self.handlers)
        delegate_blob = ",".join(self.delegates)
        return (
            f"route={self.route} "
            f"rate_limit={self.rate_limit} "
            f"ui_activity={self.ui_activity} "
            f"handlers={handler_blob} "
            f"delegates={delegate_blob}"
        )


def read_source() -> str:
    parts: list[str] = []
    for src in SRC_FILES:
        if src.exists():
            parts.append(src.read_text(encoding="utf-8"))
    if not parts:
        raise FileNotFoundError(f"No source files found: {SRC_FILES}")
    return "\n".join(parts)


def extract_routes(source: str) -> List[str]:
    rows: List[str] = []
    for path, method in ROUTE_SIGNATURE_RE.findall(source):
        if path.startswith(ROUTE_PREFIXES):
            rows.append(f"{method} {path}")
    # Keep deterministic ordering independent of registration line moves.
    return sorted(set(rows))


def find_matching_brace(source: str, open_brace_index: int) -> int:
    depth = 0
    for idx in range(open_brace_index, len(source)):
        ch = source[idx]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return idx
    raise ValueError("Unbalanced braces while parsing route lambda body")


def extract_all_route_lambda_bodies(source: str) -> Dict[str, str]:
    routes: Dict[str, str] = {}
    for match in ROUTE_LAMBDA_START_RE.finditer(source):
        route = f"{match.group(2)} {match.group(1)}"
        open_idx = match.end() - 1
        close_idx = find_matching_brace(source, open_idx)
        routes[route] = source[open_idx + 1 : close_idx]
    return routes


def extract_route_lambda_bodies(source: str) -> Dict[str, str]:
    routes = extract_all_route_lambda_bodies(source)
    out: Dict[str, str] = {}
    for route, body in routes.items():
        _method, path = route.split(" ", 1)
        if path.startswith(ROUTE_PREFIXES):
            out[route] = body
    return out


def extract_handle_method_bodies(source: str) -> Dict[str, str]:
    handlers: Dict[str, str] = {}
    for match in HANDLE_METHOD_START_RE.finditer(source):
        handler = match.group(1)
        open_idx = match.end() - 1
        close_idx = find_matching_brace(source, open_idx)
        handlers[handler] = source[open_idx + 1 : close_idx]
    return handlers


def extract_method_body(source: str, method_name: str) -> str:
    for match in METHOD_START_RE.finditer(source):
        name = match.group(1)
        if name != method_name:
            continue
        open_idx = match.end() - 1
        close_idx = find_matching_brace(source, open_idx)
        return source[open_idx + 1 : close_idx]
    return ""


def extract_policy_contract(source: str) -> List[RoutePolicy]:
    routes = extract_route_lambda_bodies(source)
    out: List[RoutePolicy] = []

    for route, body in routes.items():
        _method, path = route.split(" ", 1)
        delegates = tuple(sorted(set(DELEGATE_RE.findall(body))))
        if not delegates:
            continue

        allow_callback_policy_detection = path.startswith(POLICY_CALLBACK_PREFIXES)
        has_rate = int(
            "checkRateLimit(" in body
            or (allow_callback_policy_detection and "rateLimitCallback" in body)
        )
        has_ui = int(
            "markUiActivity(" in body
            or (allow_callback_policy_detection and "markUiActivityCallback" in body)
        )

        out.append(
            RoutePolicy(
                route=route,
                rate_limit=has_rate,
                ui_activity=has_ui,
                delegates=delegates,
            )
        )

    out.sort(key=lambda p: p.route)
    return out


def extract_shim_absence_contract(source: str) -> List[str]:
    handlers = extract_handle_method_bodies(source)
    out: List[str] = []

    for handler, body in handlers.items():
        delegates = tuple(sorted(set(DELEGATE_RE.findall(body))))
        if not delegates:
            continue
        out.append(f"handler={handler} delegates={','.join(delegates)}")

    return sorted(out)


def extract_local_handler_route_contract(source: str) -> List[str]:
    routes = extract_all_route_lambda_bodies(source)
    out: List[LocalHandlerRoutePolicy] = []

    for route in LOCAL_HANDLER_ROUTE_KEYS:
        body = routes.get(route)
        if body is None:
            continue

        handlers = tuple(sorted(set(HANDLE_CALL_RE.findall(body))))
        delegates = tuple(sorted(set(DELEGATE_RE.findall(body))))
        has_rate = int("checkRateLimit(" in body)
        has_ui = int("markUiActivity(" in body)

        out.append(
            LocalHandlerRoutePolicy(
                route=route,
                rate_limit=has_rate,
                ui_activity=has_ui,
                handlers=handlers,
                delegates=delegates,
            )
        )

    out.sort(key=lambda p: p.route)
    return [p.to_line() for p in out]


def find_delegate_placement_errors(source: str) -> List[str]:
    setup_body = extract_method_body(source, "setupWebServer")
    if not setup_body:
        return ["missing setupWebServer() definition"]

    all_delegates = Counter(DELEGATE_RE.findall(source))
    setup_delegates = Counter(DELEGATE_RE.findall(setup_body))

    extras = all_delegates - setup_delegates
    if not extras:
        return []

    errors: List[str] = []
    for delegate, count in sorted(extras.items()):
        errors.append(f"delegate outside setupWebServer: {delegate} x{count}")
    return errors


def find_api_write_guard_errors(source: str) -> List[str]:
    routes = extract_all_route_lambda_bodies(source)
    errors: List[str] = []

    for route, body in sorted(routes.items()):
        method, path = route.split(" ", 1)
        if method == "HTTP_POST" and path.startswith("/api/") and API_WRITE_GUARD_CALL not in body:
            errors.append(f"missing maintenance write guard: {route}")

    return errors


def find_api_write_guard_policy_errors(source: str) -> List[str]:
    body = extract_method_body(source, "requireMaintenanceApiWriteHeader")
    if not body:
        return ["missing requireMaintenanceApiWriteHeader() definition"]

    errors: List[str] = []
    required_fragments = {
        "header presence check": "server_.hasHeader(maintenanceApiWriteHeader())",
        "header value check": (
            "server_.header(maintenanceApiWriteHeader()) == maintenanceApiWriteHeaderValue()"
        ),
        "not-maintenance rejection": "Decision::RejectNotMaintenance",
        "header rejection": "Decision::RejectHeader",
    }
    for label, fragment in required_fragments.items():
        if fragment not in body:
            errors.append(f"maintenance write guard missing {label}")

    evaluates_mode_and_header = re.search(
        r"WifiMaintenanceWritePolicy::evaluate\s*\(\s*maintenanceBootMode_\s*,\s*"
        r"hasValidWriteHeader\s*\)",
        body,
    )
    if not evaluates_mode_and_header:
        errors.append(
            "maintenance write guard must evaluate maintenance mode and validated header together"
        )
    return errors


def read_expected_lines(path: Path) -> List[str]:
    if not path.exists():
        return []
    lines: List[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        lines.append(line)
    return lines


def write_lines(path: Path, header: str, lines: List[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = [header, ""]
    payload.extend(lines)
    payload.append("")
    path.write_text("\n".join(payload), encoding="utf-8")


def print_diff(expected: List[str], actual: List[str], label: str) -> None:
    expected_set = set(expected)
    actual_set = set(actual)

    missing = sorted(expected_set - actual_set)
    extra = sorted(actual_set - expected_set)

    print(f"[contract] {label} mismatch")
    if missing:
        print("  missing:")
        for row in missing:
            print(f"    - {row}")
    if extra:
        print("  extra:")
        for row in extra:
            print(f"    + {row}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--update",
        action="store_true",
        help="rewrite expected contract files from current source",
    )
    args = parser.parse_args()

    source = read_source()

    routes = extract_routes(source)
    policies = extract_policy_contract(source)
    policy_lines = [p.to_line() for p in policies]
    shim_absence_lines = extract_shim_absence_contract(source)
    local_handler_route_lines = extract_local_handler_route_contract(source)

    if args.update:
        write_lines(
            ROUTE_CONTRACT_FILE,
            "# WiFi API route contract (extracted ApiService endpoints)",
            routes,
        )
        write_lines(
            POLICY_CONTRACT_FILE,
            "# WiFi API route policy contract (ApiService endpoint lambdas)",
            policy_lines,
        )
        write_lines(
            SHIM_ABSENCE_CONTRACT_FILE,
            "# WiFi API shim absence contract (handle* methods must not delegate to ApiService)",
            shim_absence_lines,
        )
        write_lines(
            LOCAL_HANDLER_ROUTE_CONTRACT_FILE,
            "# WiFi API local-handler route contract (remaining non-ApiService route families)",
            local_handler_route_lines,
        )
        print(f"Updated {ROUTE_CONTRACT_FILE}")
        print(f"Updated {POLICY_CONTRACT_FILE}")
        print(f"Updated {SHIM_ABSENCE_CONTRACT_FILE}")
        print(f"Updated {LOCAL_HANDLER_ROUTE_CONTRACT_FILE}")
        return 0

    expected_routes = read_expected_lines(ROUTE_CONTRACT_FILE)
    expected_policy = read_expected_lines(POLICY_CONTRACT_FILE)
    expected_shim_absence = read_expected_lines(SHIM_ABSENCE_CONTRACT_FILE)
    expected_local_handler_routes = read_expected_lines(LOCAL_HANDLER_ROUTE_CONTRACT_FILE)

    ok = True

    if expected_routes != routes:
        print_diff(expected_routes, routes, "route")
        ok = False
    if expected_policy != policy_lines:
        print_diff(expected_policy, policy_lines, "policy")
        ok = False
    if expected_shim_absence != shim_absence_lines:
        print_diff(expected_shim_absence, shim_absence_lines, "shim-absence")
        ok = False
    if expected_local_handler_routes != local_handler_route_lines:
        print_diff(expected_local_handler_routes, local_handler_route_lines, "local-handler-route")
        ok = False

    delegate_placement_errors = find_delegate_placement_errors(source)
    if delegate_placement_errors:
        print("[contract] delegate-placement mismatch")
        for error in delegate_placement_errors:
            print(f"  - {error}")
        ok = False

    api_write_guard_errors = find_api_write_guard_errors(source)
    if api_write_guard_errors:
        print("[contract] api-write-guard mismatch")
        for error in api_write_guard_errors:
            print(f"  - {error}")
        ok = False

    api_write_guard_policy_errors = find_api_write_guard_policy_errors(source)
    if api_write_guard_policy_errors:
        print("[contract] api-write-guard-policy mismatch")
        for error in api_write_guard_policy_errors:
            print(f"  - {error}")
        ok = False

    if not ok:
        print("\nRun with --update only when intentionally changing contract.")
        return 1

    print(
        "[contract] route, policy, shim-absence, local-handler-route, "
        "delegate placement, api-write-guard, and api-write-guard-policy contracts match"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
