#!/usr/bin/env python3
"""Check WiFi API route/policy contracts for extracted ApiService endpoints.

This script enforces WiFi API route/policy invariants across extracted WiFi route files:
1) Route contract for extracted API modules stays stable (method + path).
2) Route-lambda policy contract for ApiService endpoints stays stable
   (rate-limit, UI activity mark, delegate calls).
3) WiFiManager handle* methods do not become thin ApiService shims again.
4) ApiService delegates remain bound only in setupWebServer() route registration.
5) Remaining local WiFi route families preserve route-level policy and handler bindings.
6) Every /api/ ApiService route declares all four maintenance/runtime cells exactly once.
7) Nullable ALP/GPS/OBD runtimes and maintenance state reach the service boundary.
8) Every declared ApiService delegate has a qualified invocation in a native service test.
9) WiFi client enable stages through the tested transaction before persistence.

Use --update to rewrite expected contract snapshots from current source.
"""

from __future__ import annotations

import argparse
from collections import Counter
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

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
MAINTENANCE_RUNTIME_POLICY_CONTRACT_FILE = (
    ROOT / "test" / "contracts" / "wifi_maintenance_runtime_policy_contract.txt"
)
NATIVE_API_SERVICE_TEST_GLOBS = (
    "test/test_*api_service/*.cpp",
    "test/test_api_maintenance_runtime_matrix/*.cpp",
)
API_WRITE_GUARD_CALL = "requireMaintenanceApiWriteHeader()"
VALID_RUNTIME_FAMILIES = frozenset(
    ("none", "alp", "display", "gps", "obd", "storage", "system", "v1", "wifi")
)
MATRIX_CELL_NAMES = (
    "normal_absent",
    "normal_present",
    "maintenance_absent",
    "maintenance_present",
)
VALID_MATRIX_EXPECTATIONS = frozenset(
    ("serve", "runtime_unavailable", "maintenance_conflict", "maintenance_required")
)
# Closed shapes keep the manual matrix reviewable. A new policy shape must be
# named here deliberately instead of being accepted as an arbitrary combination.
VALID_MATRIX_SHAPES = frozenset(
    (
        ("serve", "serve", "serve", "serve"),
        ("runtime_unavailable", "serve", "serve", "serve"),
        (
            "runtime_unavailable",
            "serve",
            "maintenance_conflict",
            "maintenance_conflict",
        ),
        (
            "maintenance_required",
            "maintenance_required",
            "runtime_unavailable",
            "serve",
        ),
    )
)

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
MAINTENANCE_RUNTIME_POLICY_RE = re.compile(
    r"^route=(HTTP_[A-Z]+ /api/\S+) runtime=(\S+) "
    r"normal_absent=(\S+) normal_present=(\S+) "
    r"maintenance_absent=(\S+) maintenance_present=(\S+) delegates=(\S+)$"
)
QUALIFIED_DELEGATE_CALL_RE = re.compile(
    r"(?<![A-Za-z0-9_:])([A-Za-z0-9_]+ApiService::[A-Za-z0-9_]+)\s*\("
)


@dataclass(frozen=True)
class NullableRuntimeRouteRequirement:
    route: str
    delegate: str
    runtime_symbol: str
    maintenance_source: str


NULLABLE_RUNTIME_ROUTE_REQUIREMENTS = (
    NullableRuntimeRouteRequirement(
        "HTTP_GET /api/alp/status",
        "AlpApiService::handleApiStatus",
        "alpRuntime_",
        "direct",
    ),
    NullableRuntimeRouteRequirement(
        "HTTP_POST /api/gps/config",
        "GpsApiService::handleApiConfigSave",
        "gpsRuntime_",
        "local_runtime",
    ),
    NullableRuntimeRouteRequirement(
        "HTTP_GET /api/gps/status",
        "GpsApiService::handleApiStatus",
        "gpsRuntime_",
        "local_runtime",
    ),
    NullableRuntimeRouteRequirement(
        "HTTP_GET /api/obd/status",
        "ObdApiService::handleApiStatus",
        "obdRuntime_",
        "obd_factory",
    ),
    NullableRuntimeRouteRequirement(
        "HTTP_GET /api/obd/devices",
        "ObdApiService::handleApiDevicesList",
        "obdRuntime_",
        "obd_factory",
    ),
    NullableRuntimeRouteRequirement(
        "HTTP_POST /api/obd/scan",
        "ObdApiService::handleApiScan",
        "obdRuntime_",
        "obd_factory",
    ),
    NullableRuntimeRouteRequirement(
        "HTTP_POST /api/obd/forget",
        "ObdApiService::handleApiForget",
        "obdRuntime_",
        "obd_factory",
    ),
    NullableRuntimeRouteRequirement(
        "HTTP_POST /api/obd/config",
        "ObdApiService::handleApiConfig",
        "obdRuntime_",
        "obd_factory",
    ),
)


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


@dataclass(frozen=True)
class MaintenanceRuntimePolicy:
    route: str
    runtime: str
    normal_absent: str
    normal_present: str
    maintenance_absent: str
    maintenance_present: str
    delegates: Tuple[str, ...]

    @property
    def matrix(self) -> Tuple[str, str, str, str]:
        return (
            self.normal_absent,
            self.normal_present,
            self.maintenance_absent,
            self.maintenance_present,
        )

    def to_line(self) -> str:
        return (
            f"route={self.route} "
            f"runtime={self.runtime} "
            f"normal_absent={self.normal_absent} "
            f"normal_present={self.normal_present} "
            f"maintenance_absent={self.maintenance_absent} "
            f"maintenance_present={self.maintenance_present} "
            f"delegates={','.join(self.delegates)}"
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


def extract_api_service_route_rows(source: str) -> List[Tuple[str, Tuple[str, ...]]]:
    out: List[Tuple[str, Tuple[str, ...]]] = []
    for match in ROUTE_LAMBDA_START_RE.finditer(source):
        route = f"{match.group(2)} {match.group(1)}"
        _method, path = route.split(" ", 1)
        if not path.startswith("/api/"):
            continue
        open_idx = match.end() - 1
        close_idx = find_matching_brace(source, open_idx)
        delegates = tuple(sorted(set(DELEGATE_RE.findall(source[open_idx + 1 : close_idx]))))
        if delegates:
            out.append((route, delegates))
    return out


def extract_api_service_routes(source: str) -> Dict[str, Tuple[str, ...]]:
    return dict(extract_api_service_route_rows(source))


def parse_maintenance_runtime_policy(
    lines: List[str],
) -> Tuple[Dict[str, MaintenanceRuntimePolicy], List[str]]:
    policies: Dict[str, MaintenanceRuntimePolicy] = {}
    errors: List[str] = []

    for line_number, line in enumerate(lines, start=1):
        match = MAINTENANCE_RUNTIME_POLICY_RE.fullmatch(line)
        if not match:
            errors.append(f"malformed policy row {line_number}: {line}")
            continue

        (
            route,
            runtime,
            normal_absent,
            normal_present,
            maintenance_absent,
            maintenance_present,
            delegate_blob,
        ) = match.groups()
        if route in policies:
            errors.append(f"duplicate policy row: {route}")
            continue
        if runtime not in VALID_RUNTIME_FAMILIES:
            errors.append(f"invalid runtime family for {route}: {runtime}")

        matrix = (normal_absent, normal_present, maintenance_absent, maintenance_present)
        for cell_name, expectation in zip(MATRIX_CELL_NAMES, matrix):
            if expectation not in VALID_MATRIX_EXPECTATIONS:
                errors.append(
                    f"invalid {cell_name} expectation for {route}: {expectation}"
                )
        if all(expectation in VALID_MATRIX_EXPECTATIONS for expectation in matrix):
            if matrix not in VALID_MATRIX_SHAPES:
                errors.append(
                    f"unsupported four-cell matrix for {route}: {','.join(matrix)}"
                )
            if runtime == "none" and (
                normal_absent != normal_present
                or maintenance_absent != maintenance_present
            ):
                errors.append(
                    f"runtime=none policy varies by runtime presence for {route}"
                )

        delegates = tuple(delegate_blob.split(","))
        if len(delegates) != len(set(delegates)):
            errors.append(f"duplicate delegate in policy row: {route}")
        policies[route] = MaintenanceRuntimePolicy(
            route,
            runtime,
            normal_absent,
            normal_present,
            maintenance_absent,
            maintenance_present,
            delegates,
        )

    return policies, errors


def find_maintenance_runtime_policy_errors(source: str, lines: List[str]) -> List[str]:
    actual_rows = extract_api_service_route_rows(source)
    actual_route_counts = Counter(route for route, _delegates in actual_rows)
    actual_routes = dict(actual_rows)
    policies, errors = parse_maintenance_runtime_policy(lines)

    for route, count in sorted(actual_route_counts.items()):
        if count > 1:
            errors.append(f"duplicate ApiService route registration: {route} x{count}")
    for route in sorted(actual_routes.keys() - policies.keys()):
        errors.append(f"missing policy row: {route}")
    for route in sorted(policies.keys() - actual_routes.keys()):
        errors.append(f"extra policy row: {route}")
    for route in sorted(actual_routes.keys() & policies.keys()):
        if policies[route].delegates != actual_routes[route]:
            errors.append(
                f"delegate mismatch for {route}: expected {','.join(actual_routes[route])}; "
                f"contract has {','.join(policies[route].delegates)}"
            )

    return errors


def native_api_service_test_paths(root: Path = ROOT) -> List[Path]:
    """Return only native service-test translation units allowed to prove coverage."""

    return sorted(
        {
            path
            for pattern in NATIVE_API_SERVICE_TEST_GLOBS
            for path in root.glob(pattern)
            if path.is_file()
        }
    )


def read_native_api_service_test_sources(root: Path = ROOT) -> Dict[str, str]:
    return {
        path.relative_to(root).as_posix(): path.read_text(encoding="utf-8")
        for path in native_api_service_test_paths(root)
    }


def _without_preprocessor_directives(source: str) -> str:
    """Remove directives so an include or macro cannot masquerade as execution."""

    lines = source.splitlines(keepends=True)
    out: List[str] = []
    in_directive = False
    for line in lines:
        stripped = line.lstrip()
        if in_directive or stripped.startswith("#"):
            out.append("\n" if line.endswith("\n") else "")
            in_directive = line.rstrip().endswith("\\")
        else:
            out.append(line)
    return "".join(out)


def _find_matching_parenthesis(source: str, open_index: int) -> int:
    depth = 0
    quote = ""
    escaped = False
    for index in range(open_index, len(source)):
        char = source[index]
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = ""
            continue
        if char in ('"', "'"):
            quote = char
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return index
    return -1


def _is_qualified_delegate_definition(source: str, match: re.Match[str]) -> bool:
    """Reject an out-of-class definition while retaining calls in test bodies."""

    open_index = source.find("(", match.end(1))
    close_index = _find_matching_parenthesis(source, open_index)
    if close_index < 0:
        return False

    suffix = source[close_index + 1 :]
    definition_suffix = re.match(
        r"\s*(?:(?:const|noexcept|override|final)\b\s*)*(?:->[^\{;]+\s*)?\{",
        suffix,
    )
    return definition_suffix is not None


def extract_qualified_delegate_invocations(source: str) -> set[str]:
    """Extract explicit ApiService calls, excluding includes and definitions."""

    source = _without_preprocessor_directives(source)
    invocations: set[str] = set()
    for match in QUALIFIED_DELEGATE_CALL_RE.finditer(source):
        if not _is_qualified_delegate_definition(source, match):
            invocations.add(match.group(1))
    return invocations


def find_native_delegate_coverage_errors(
    policies: Dict[str, MaintenanceRuntimePolicy], test_sources: Dict[str, str]
) -> List[str]:
    required = {
        delegate for policy in policies.values() for delegate in policy.delegates
    }
    invoked = {
        delegate
        for source in test_sources.values()
        for delegate in extract_qualified_delegate_invocations(source)
    }
    return [
        f"missing qualified native ApiService invocation: {delegate}"
        for delegate in sorted(required - invoked)
    ]


def extract_call_arguments(body: str, qualified_name: str) -> Tuple[str, int] | None:
    call_index = body.find(qualified_name)
    if call_index < 0:
        return None
    open_index = body.find("(", call_index + len(qualified_name))
    if open_index < 0:
        return None

    depth = 0
    for index in range(open_index, len(body)):
        char = body[index]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return body[open_index + 1 : index], call_index
    return None


def split_top_level_arguments(arguments: str) -> Tuple[str, ...]:
    """Split a C++ call while preserving commas inside lambdas/calls/strings."""

    out: List[str] = []
    start = 0
    paren_depth = 0
    bracket_depth = 0
    brace_depth = 0
    quote = ""
    escaped = False

    for index, char in enumerate(arguments):
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = ""
            continue
        if char in ('"', "'"):
            quote = char
        elif char == "(":
            paren_depth += 1
        elif char == ")":
            paren_depth -= 1
        elif char == "[":
            bracket_depth += 1
        elif char == "]":
            bracket_depth -= 1
        elif char == "{":
            brace_depth += 1
        elif char == "}":
            brace_depth -= 1
        elif char == "," and not (paren_depth or bracket_depth or brace_depth):
            out.append(arguments[start:index].strip())
            start = index + 1

    out.append(arguments[start:].strip())
    return tuple(out)


def extract_obd_runtime_factory_body(source: str) -> str:
    match = re.search(
        r"ObdApiService::Runtime\s+WiFiManager::makeObdRuntime\s*\(\s*\)\s*\{",
        source,
    )
    if not match:
        return ""
    open_index = match.end() - 1
    close_index = find_matching_brace(source, open_index)
    return source[open_index + 1 : close_index]


def find_nullable_runtime_route_composition_errors(source: str) -> List[str]:
    """Pin nullable-runtime and maintenance precedence at route composition."""

    routes = extract_all_route_lambda_bodies(source)
    errors: List[str] = []

    for requirement in NULLABLE_RUNTIME_ROUTE_REQUIREMENTS:
        body = routes.get(requirement.route)
        if body is None:
            errors.append(f"missing nullable-runtime route: {requirement.route}")
            continue

        call = extract_call_arguments(body, requirement.delegate)
        if call is None:
            errors.append(
                f"missing nullable-runtime delegate for {requirement.route}: "
                f"{requirement.delegate}"
            )
            continue
        arguments, call_index = call
        call_arguments = split_top_level_arguments(arguments)

        if requirement.runtime_symbol not in call_arguments:
            errors.append(
                f"nullable runtime not passed directly to service for {requirement.route}: "
                f"{requirement.runtime_symbol}"
            )
        if any(
            re.search(rf"\*\s*{re.escape(requirement.runtime_symbol)}\b", argument)
            for argument in call_arguments
        ):
            errors.append(
                f"runtime dereferenced before service for {requirement.route}: "
                f"{requirement.runtime_symbol}"
            )
        if re.search(rf"\b{re.escape(requirement.runtime_symbol)}\b", body[:call_index]):
            errors.append(
                f"runtime accessed before service for {requirement.route}: "
                f"{requirement.runtime_symbol}"
            )

        if requirement.maintenance_source == "direct":
            if "mainRuntimeState.maintenanceBootActive" not in call_arguments:
                errors.append(
                    f"maintenance state not passed to service for {requirement.route}"
                )
        elif requirement.maintenance_source == "local_runtime":
            assignment = re.search(
                r"\br\.maintenanceBootActive\s*=\s*"
                r"mainRuntimeState\.maintenanceBootActive\s*;",
                body[:call_index],
            )
            if not assignment or "r" not in call_arguments:
                errors.append(
                    f"maintenance state not populated before service for {requirement.route}"
                )
        elif requirement.maintenance_source == "obd_factory":
            if "makeObdRuntime()" not in call_arguments:
                errors.append(
                    f"maintenance runtime factory not passed to service for {requirement.route}"
                )

    obd_factory_body = extract_obd_runtime_factory_body(source)
    if not obd_factory_body:
        errors.append("missing makeObdRuntime() definition")
    else:
        maintenance_assignment = re.search(
            r"\br\.maintenanceBootActive\s*=\s*"
            r"mainRuntimeState\.maintenanceBootActive\s*;",
            obd_factory_body,
        )
        return_match = re.search(r"\breturn\s+r\s*;", obd_factory_body)
        if not maintenance_assignment or not return_match or maintenance_assignment.start() > return_match.start():
            errors.append("makeObdRuntime() must propagate maintenance state before return")

    return errors


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


def extract_assigned_lambda_body(source: str, assignment: str) -> Optional[str]:
    """Return the body of a lambda assigned to a named transaction callback."""

    match = re.search(
        rf"{re.escape(assignment)}\s*=\s*\[[^\]]*\]\s*\([^)]*\)\s*"
        rf"(?:mutable\s*)?(?:->\s*[^{{]+)?\s*{{",
        source,
    )
    if not match:
        return None
    open_index = match.end() - 1
    close_index = find_matching_brace(source, open_index)
    return source[open_index + 1 : close_index]


def normalize_cpp_tokens(source: str) -> str:
    """Collapse formatting so a semantic guard can pin a small C++ seam."""

    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    source = re.sub(r"//[^\n]*", "", source)
    return re.sub(r"\s+", "", source)


WIFI_ENABLE_TRANSACTION_BODY_CONTRACT = """
    struct EnableContext {
        WiFiManager* manager;
        WifiClientState priorState;
        int priorConnectedSlotIndex;
    };

    const bool wasEnabled = settingsManager.get().wifiClientEnabled;
    EnableContext transaction{this, wifiClientState_, currentConnectedSlotIndex_};

    WifiClientEnableTransaction::Runtime runtime;
    runtime.ctx = &transaction;
    runtime.persistedEnabled = wasEnabled;
    runtime.lifecycleAdmitted = wifiClientState_ == WIFI_CLIENT_CONNECTING ||
                                wifiClientState_ == WIFI_CLIENT_CONNECTED ||
                                maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::SCANNING ||
                                maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::CONNECTING;
    runtime.attemptStart = [](void* ctx) {
        auto* transaction = static_cast<EnableContext*>(ctx);
        WiFiManager* self = transaction->manager;
        if (self->maintenanceBootMode_) {
            if (self->beginMaintenanceAutoConnectScan(true)) {
                return true;
            }
            return !settingsManager.get().hasConfiguredWifiStaSlot();
        }

        const String savedSsid = settingsManager.get().wifiClientSSID;
        if (savedSsid.length() == 0) {
            self->wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
            self->currentConnectedSlotIndex_ = -1;
            return true;
        }

        if (self->connectToNetwork(savedSsid, settingsManager.getWifiClientPassword())) {
            return true;
        }

        self->wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
        self->currentConnectedSlotIndex_ = -1;
        return false;
    };
    runtime.rollbackFailedStart = [](void* ctx) {
        auto* transaction = static_cast<EnableContext*>(ctx);
        transaction->manager->wifiClientState_ = transaction->priorState;
        transaction->manager->currentConnectedSlotIndex_ = transaction->priorConnectedSlotIndex;
    };
    runtime.commitEnabled = [](void*) { settingsManager.setWifiClientEnabled(true); };
    return WifiClientEnableTransaction::execute(runtime);
"""


def find_wifi_enable_transaction_errors(source: str) -> List[str]:
    """Pin the unhosted WiFiManager method to the linked transaction seam."""

    body = extract_method_body(source, "enableWifiClientFromSavedCredentials")
    if not body:
        return ["missing enableWifiClientFromSavedCredentials() definition"]

    errors: List[str] = []
    if normalize_cpp_tokens(body) != normalize_cpp_tokens(
        WIFI_ENABLE_TRANSACTION_BODY_CONTRACT
    ):
        errors.append("wifi enable transaction differs from reviewed manager wiring")

    required_fragments = {
        "transaction start callback": "runtime.attemptStart",
        "transaction rollback callback": "runtime.rollbackFailedStart",
        "transaction commit callback": "runtime.commitEnabled",
        "transaction execution": "WifiClientEnableTransaction::execute(runtime)",
    }
    positions: Dict[str, int] = {}
    for label, fragment in required_fragments.items():
        position = body.find(fragment)
        positions[label] = position
        if position < 0:
            errors.append(f"wifi enable transaction missing {label}")

    persisted_snapshots = re.findall(
        r"const\s+bool\s+wasEnabled\s*=\s*"
        r"settingsManager\.get\(\)\.wifiClientEnabled\s*;",
        body,
    )
    persisted_assignments = re.findall(
        r"runtime\.persistedEnabled\s*=\s*(.*?);", body, re.DOTALL
    )
    if (
        len(persisted_snapshots) != 1
        or len(persisted_assignments) != 1
        or normalize_cpp_tokens(persisted_assignments[0]) != "wasEnabled"
    ):
        errors.append("wifi enable transaction missing persisted enable snapshot")

    prior_runtime = re.search(
        r"EnableContext\s+transaction\s*{\s*this\s*,\s*wifiClientState_\s*,\s*"
        r"currentConnectedSlotIndex_\s*}\s*;",
        body,
    )
    if not prior_runtime:
        errors.append("wifi enable transaction missing prior runtime snapshot")

    context_assignments = re.findall(
        r"runtime\.ctx\s*=\s*(.*?);", body, re.DOTALL
    )
    if (
        len(context_assignments) != 1
        or normalize_cpp_tokens(context_assignments[0]) != "&transaction"
    ):
        errors.append("wifi enable transaction missing canonical transaction context")

    lifecycle_assignments = re.findall(
        r"runtime\.lifecycleAdmitted\s*=\s*(.*?);", body, re.DOTALL
    )
    expected_lifecycle_assignment = normalize_cpp_tokens(
        """
        wifiClientState_ == WIFI_CLIENT_CONNECTING ||
        wifiClientState_ == WIFI_CLIENT_CONNECTED ||
        maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::SCANNING ||
        maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::CONNECTING
        """
    )
    if (
        len(lifecycle_assignments) != 1
        or normalize_cpp_tokens(lifecycle_assignments[0])
        != expected_lifecycle_assignment
    ):
        errors.append("wifi enable transaction missing lifecycle admission snapshot")

    callback_assignments = {
        "start": "runtime.attemptStart",
        "rollback": "runtime.rollbackFailedStart",
        "commit": "runtime.commitEnabled",
    }
    callback_bodies = {
        label: extract_assigned_lambda_body(body, assignment)
        for label, assignment in callback_assignments.items()
    }
    for label, callback_body in callback_bodies.items():
        assignment_count = len(
            re.findall(
                rf"{re.escape(callback_assignments[label])}\s*=", body
            )
        )
        if assignment_count != 1:
            errors.append(
                f"wifi enable transaction {label} callback must be assigned exactly once"
            )
        if callback_body is None:
            errors.append(f"wifi enable transaction {label} callback must remain a lambda")

    start_body = callback_bodies["start"] or ""
    rollback_body = callback_bodies["rollback"] or ""
    commit_body = callback_bodies["commit"] or ""

    explicit_admission = "beginMaintenanceAutoConnectScan(true)"
    if start_body.count(explicit_admission) != 1 or body.count(explicit_admission) != 1:
        errors.append("wifi enable transaction missing explicit maintenance enable admission")
    expected_start_body = normalize_cpp_tokens(
        """
        auto* transaction = static_cast<EnableContext*>(ctx);
        WiFiManager* self = transaction->manager;
        if (self->maintenanceBootMode_) {
            if (self->beginMaintenanceAutoConnectScan(true)) {
                return true;
            }
            return !settingsManager.get().hasConfiguredWifiStaSlot();
        }
        const String savedSsid = settingsManager.get().wifiClientSSID;
        if (savedSsid.length() == 0) {
            self->wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
            self->currentConnectedSlotIndex_ = -1;
            return true;
        }
        if (self->connectToNetwork(savedSsid, settingsManager.getWifiClientPassword())) {
            return true;
        }
        self->wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
        self->currentConnectedSlotIndex_ = -1;
        return false;
        """
    )
    if normalize_cpp_tokens(start_body) != expected_start_body:
        errors.append("wifi enable start callback differs from reviewed admission flow")

    rollback_requirements = {
        "client state": (
            r"wifiClientState_\s*=\s*transaction->priorState\s*;"
        ),
        "connected slot": (
            r"currentConnectedSlotIndex_\s*=\s*"
            r"transaction->priorConnectedSlotIndex\s*;"
        ),
    }
    for label, pattern in rollback_requirements.items():
        if not re.search(pattern, rollback_body):
            errors.append(f"wifi enable rollback missing prior {label} restoration")
    expected_rollback_body = normalize_cpp_tokens(
        """
        auto* transaction = static_cast<EnableContext*>(ctx);
        transaction->manager->wifiClientState_ = transaction->priorState;
        transaction->manager->currentConnectedSlotIndex_ =
            transaction->priorConnectedSlotIndex;
        """
    )
    if normalize_cpp_tokens(rollback_body) != expected_rollback_body:
        errors.append(
            "wifi enable rollback must remain an unconditional prior-state restoration"
        )

    setter = "settingsManager.setWifiClientEnabled(true)"
    setter_positions = [match.start() for match in re.finditer(re.escape(setter), body)]
    if len(setter_positions) != 1:
        errors.append(
            "wifi enable transaction must contain exactly one enabled persistence commit"
        )
    if normalize_cpp_tokens(commit_body) != normalize_cpp_tokens(f"{setter};"):
        errors.append("wifi enable persistence must remain inside the commit callback")

    ordered_labels = (
        "transaction start callback",
        "transaction rollback callback",
        "transaction commit callback",
        "transaction execution",
    )
    ordered_positions = [positions[label] for label in ordered_labels]
    if all(position >= 0 for position in ordered_positions) and ordered_positions != sorted(
        ordered_positions
    ):
        errors.append("wifi enable transaction callbacks must be wired before execution")

    if body.count("WifiClientEnableTransaction::execute(runtime)") != 1:
        errors.append("wifi enable transaction must execute exactly once")

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
    maintenance_runtime_policy_lines = read_expected_lines(MAINTENANCE_RUNTIME_POLICY_CONTRACT_FILE)
    maintenance_runtime_policy_errors = find_maintenance_runtime_policy_errors(
        source, maintenance_runtime_policy_lines
    )
    maintenance_runtime_policies, _policy_parse_errors = (
        parse_maintenance_runtime_policy(maintenance_runtime_policy_lines)
    )
    native_delegate_coverage_errors = find_native_delegate_coverage_errors(
        maintenance_runtime_policies, read_native_api_service_test_sources()
    )
    nullable_runtime_route_composition_errors = (
        find_nullable_runtime_route_composition_errors(source)
    )
    wifi_enable_transaction_errors = find_wifi_enable_transaction_errors(source)

    if args.update:
        if (
            maintenance_runtime_policy_errors
            or nullable_runtime_route_composition_errors
            or native_delegate_coverage_errors
            or wifi_enable_transaction_errors
        ):
            if maintenance_runtime_policy_errors:
                print("[contract] maintenance-runtime-policy mismatch")
                for error in maintenance_runtime_policy_errors:
                    print(f"  - {error}")
            if nullable_runtime_route_composition_errors:
                print("[contract] nullable-runtime-route-composition mismatch")
                for error in nullable_runtime_route_composition_errors:
                    print(f"  - {error}")
            if native_delegate_coverage_errors:
                print("[contract] native-delegate-coverage mismatch")
                for error in native_delegate_coverage_errors:
                    print(f"  - {error}")
            if wifi_enable_transaction_errors:
                print("[contract] wifi-enable-transaction mismatch")
                for error in wifi_enable_transaction_errors:
                    print(f"  - {error}")
            print(
                "\nMaintenance/runtime policy is manual; declare behavior for every route "
                "and preserve nullable-runtime composition plus qualified native delegate "
                "coverage before updating snapshots."
            )
            return 1

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
        maintenance_runtime_policies, _errors = parse_maintenance_runtime_policy(
            maintenance_runtime_policy_lines
        )
        write_lines(
            MAINTENANCE_RUNTIME_POLICY_CONTRACT_FILE,
            "# WiFi API four-cell maintenance/runtime policy contract (manual)",
            [maintenance_runtime_policies[route].to_line() for route in sorted(maintenance_runtime_policies)],
        )
        print(f"Updated {ROUTE_CONTRACT_FILE}")
        print(f"Updated {POLICY_CONTRACT_FILE}")
        print(f"Updated {SHIM_ABSENCE_CONTRACT_FILE}")
        print(f"Updated {LOCAL_HANDLER_ROUTE_CONTRACT_FILE}")
        print(f"Updated {MAINTENANCE_RUNTIME_POLICY_CONTRACT_FILE}")
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

    if maintenance_runtime_policy_errors:
        print("[contract] maintenance-runtime-policy mismatch")
        for error in maintenance_runtime_policy_errors:
            print(f"  - {error}")
        ok = False

    if nullable_runtime_route_composition_errors:
        print("[contract] nullable-runtime-route-composition mismatch")
        for error in nullable_runtime_route_composition_errors:
            print(f"  - {error}")
        ok = False

    if native_delegate_coverage_errors:
        print("[contract] native-delegate-coverage mismatch")
        for error in native_delegate_coverage_errors:
            print(f"  - {error}")
        ok = False

    if wifi_enable_transaction_errors:
        print("[contract] wifi-enable-transaction mismatch")
        for error in wifi_enable_transaction_errors:
            print(f"  - {error}")
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
        "maintenance-runtime-policy, nullable-runtime-route-composition, native-delegate-"
        "coverage, wifi-enable-transaction, delegate placement, api-write-guard, and "
        "api-write-guard-policy contracts match"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
