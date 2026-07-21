#!/usr/bin/env python3
"""Generate deterministic frontend fixtures from real native API handlers."""

from __future__ import annotations

import argparse
import difflib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
FIXTURE_PATH = ROOT / "interface" / "src" / "test" / "fixtures" / "wifi-api.json"
FRONTEND_SOURCE_ROOT = ROOT / "interface" / "src"
WIFI_ROUTES_PATH = ROOT / "src" / "wifi_routes.cpp"
EMITTER_SUITES = (
    "test_wifi_api_fixture_emitter",
    "test_obd_api_fixture_emitter",
    "test_wifi_api_fixture_remaining_emitter",
)
FIXTURE_MARKER = "V1_WIFI_API_FIXTURE="
ROUTE_KEY_RE = re.compile(r"^[A-Z]+ /\S+$")
API_LITERAL_RE = re.compile(r"(?P<quote>['\"`])(?P<value>/api/.*?)(?P=quote)", re.DOTALL)
API_CONSTANT_RE = re.compile(
    r"(?:export\s+)?const\s+(?P<name>[A-Za-z_$][\w$]*)\s*=\s*"
    r"(?P<quote>['\"`])(?P<value>/api/.*?)(?P=quote)",
    re.DOTALL,
)
RETURN_API_RE = re.compile(
    r"\breturn\s+(?P<quote>['\"`])(?P<value>/api/.*?)(?P=quote)", re.DOTALL
)
API_CALL_RE = re.compile(r"\b(?P<name>fetchWithTimeout|postSettingsForm)\s*\(")
REGISTERED_ROUTE_RE = re.compile(
    r"\bserver_\.on\(\s*\"(?P<path>/api/[^\"]+)\"\s*,\s*HTTP_(?P<method>[A-Z]+)"
)
SUPPORTED_HTTP_METHODS = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"}


class FixtureGenerationError(RuntimeError):
    """Raised when the native emitter does not produce a valid fixture."""


def validate_fixture(fixture: Any) -> dict[str, Any]:
    if not isinstance(fixture, dict) or fixture.get("schemaVersion") != 1:
        raise FixtureGenerationError("fixture must be an object with schemaVersion=1")

    scenarios = fixture.get("scenarios")
    if not isinstance(scenarios, dict) or not scenarios:
        raise FixtureGenerationError("fixture must contain at least one scenario")

    for scenario_name, routes in scenarios.items():
        if not isinstance(scenario_name, str) or not scenario_name:
            raise FixtureGenerationError("scenario names must be non-empty strings")
        if not isinstance(routes, dict) or not routes:
            raise FixtureGenerationError(f"scenario {scenario_name!r} has no routes")

        for route_key, responses in routes.items():
            if not isinstance(route_key, str) or not ROUTE_KEY_RE.fullmatch(route_key):
                raise FixtureGenerationError(f"invalid route key {route_key!r}")
            if "?" in route_key or "#" in route_key:
                raise FixtureGenerationError(
                    f"route key {route_key!r} must contain a query-normalized pathname"
                )
            if not isinstance(responses, list) or not responses:
                raise FixtureGenerationError(f"{scenario_name}/{route_key} has no responses")

            for index, response in enumerate(responses):
                label = f"{scenario_name}/{route_key}[{index}]"
                if not isinstance(response, dict):
                    raise FixtureGenerationError(f"{label} must be an object")
                status = response.get("status")
                if not isinstance(status, int) or isinstance(status, bool) or not 100 <= status <= 599:
                    raise FixtureGenerationError(f"{label} has invalid HTTP status")
                content_type = response.get("contentType")
                if not isinstance(content_type, str) or not content_type:
                    raise FixtureGenerationError(f"{label} has invalid contentType")
                if "body" not in response:
                    raise FixtureGenerationError(f"{label} is missing body")

    return fixture


def normalize_emitter_fixture(fixture: Any) -> dict[str, Any]:
    if not isinstance(fixture, dict) or fixture.get("schemaVersion") != 1:
        raise FixtureGenerationError("emitter output must be an object with schemaVersion=1")
    captures = fixture.get("captures")
    if not isinstance(captures, list) or not captures:
        return validate_fixture(fixture)

    scenarios: dict[str, dict[str, list[dict[str, Any]]]] = {}
    for index, capture in enumerate(captures):
        if not isinstance(capture, dict):
            raise FixtureGenerationError(f"capture[{index}] must be an object")
        scenario = capture.get("scenario")
        route = capture.get("route")
        if not isinstance(scenario, str) or not scenario:
            raise FixtureGenerationError(f"capture[{index}] has invalid scenario")
        if not isinstance(route, str) or not ROUTE_KEY_RE.fullmatch(route):
            raise FixtureGenerationError(f"capture[{index}] has invalid route")
        response = {key: value for key, value in capture.items() if key not in {"scenario", "route"}}
        scenarios.setdefault(scenario, {}).setdefault(route, []).append(response)

    return validate_fixture({"schemaVersion": 1, "scenarios": scenarios})


def extract_fixture(output: str) -> dict[str, Any]:
    payloads = []
    for line in output.splitlines():
        marker_pos = line.find(FIXTURE_MARKER)
        if marker_pos >= 0:
            payloads.append(line[marker_pos + len(FIXTURE_MARKER) :].strip())

    if len(payloads) != 1:
        raise FixtureGenerationError(
            f"expected exactly one {FIXTURE_MARKER!r} record, found {len(payloads)}"
        )

    try:
        fixture = json.loads(payloads[0])
    except json.JSONDecodeError as error:
        raise FixtureGenerationError(f"native emitter returned invalid JSON: {error}") from error
    return normalize_emitter_fixture(fixture)


def canonical_fixture_text(fixture: dict[str, Any]) -> str:
    validate_fixture(fixture)
    # Match the interface JSON-specific Prettier override so the generated
    # artifact is simultaneously canonical and formatting-check clean.
    return json.dumps(fixture, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def merge_fixtures(fixtures: list[dict[str, Any]]) -> dict[str, Any]:
    merged_scenarios: dict[str, Any] = {}
    for fixture in fixtures:
        normalized = normalize_emitter_fixture(fixture)
        for scenario_name, routes in normalized["scenarios"].items():
            if scenario_name in merged_scenarios:
                raise FixtureGenerationError(f"duplicate emitted scenario {scenario_name!r}")
            merged_scenarios[scenario_name] = routes
    return validate_fixture({"schemaVersion": 1, "scenarios": merged_scenarios})


def _frontend_source_files(source_root: Path) -> list[Path]:
    files = []
    for path in source_root.rglob("*"):
        if not path.is_file() or path.suffix not in {".js", ".svelte", ".ts", ".tsx"}:
            continue
        relative = path.relative_to(source_root)
        if "test" in relative.parts or "__tests__" in relative.parts or ".test." in path.name:
            continue
        files.append(path)
    return sorted(files)


def _balanced_call_body(source: str, open_paren: int) -> tuple[str, int] | None:
    depth = 0
    quote: str | None = None
    escaped = False
    for index in range(open_paren, len(source)):
        char = source[index]
        if quote is not None:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
            continue
        if char in {"'", '"', "`"}:
            quote = char
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return source[open_paren + 1 : index], open_paren + 1
    return None


def _split_call_arguments(body: str) -> list[tuple[str, int, int]]:
    arguments = []
    start = 0
    round_depth = square_depth = brace_depth = 0
    quote: str | None = None
    escaped = False
    for index, char in enumerate(body):
        if quote is not None:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
            continue
        if char in {"'", '"', "`"}:
            quote = char
        elif char == "(":
            round_depth += 1
        elif char == ")":
            round_depth -= 1
        elif char == "[":
            square_depth += 1
        elif char == "]":
            square_depth -= 1
        elif char == "{":
            brace_depth += 1
        elif char == "}":
            brace_depth -= 1
        elif char == "," and round_depth == square_depth == brace_depth == 0:
            arguments.append((body[start:index], start, index))
            start = index + 1
    arguments.append((body[start:], start, len(body)))
    return arguments


def _normalized_api_path(endpoint: str) -> str:
    path = endpoint.split("#", 1)[0].split("?", 1)[0]
    if not re.fullmatch(r"/api/[^\s?#]*", path):
        raise FixtureGenerationError(f"cannot normalize frontend API endpoint {endpoint!r}")
    return path


def _fetch_method(arguments: list[tuple[str, int, int]], source_label: str) -> str:
    if len(arguments) < 2 or not arguments[1][0].strip():
        return "GET"
    init = arguments[1][0].strip()
    if not (init.startswith("{") and init.endswith("}")):
        raise FixtureGenerationError(
            f"dynamic fetch init is unsupported for frontend API route in {source_label}"
        )

    method_value: str | None = None
    for property_text, _, _ in _split_call_arguments(init[1:-1]):
        property_text = property_text.strip()
        if property_text.startswith("..."):
            raise FixtureGenerationError(
                f"dynamic fetch init spread is unsupported for frontend API route in {source_label}"
            )
        if property_text == "method":
            raise FixtureGenerationError(
                f"dynamic fetch method is unsupported for frontend API route in {source_label}"
            )
        if property_text.startswith("["):
            static_computed = re.match(
                r"^\[\s*(?P<quote>['\"])(?P<name>.*?)\1\s*\]\s*:",
                property_text,
                re.DOTALL,
            )
            if not static_computed or static_computed.group("name") == "method":
                raise FixtureGenerationError(
                    f"dynamic fetch method is unsupported for frontend API route in {source_label}"
                )
            continue
        property_match = re.fullmatch(
            r"\s*(?:method|['\"]method['\"])\s*:\s*(?P<value>.*?)\s*",
            property_text,
            re.DOTALL,
        )
        if property_match:
            method_value = property_match.group("value")
    if method_value is None:
        return "GET"

    literal_method = re.fullmatch(r"['\"](?P<method>[A-Za-z]+)['\"]", method_value)
    if not literal_method:
        raise FixtureGenerationError(
            f"dynamic fetch method is unsupported for frontend API route in {source_label}"
        )
    method = literal_method.group("method").upper()
    if method not in SUPPORTED_HTTP_METHODS:
        raise FixtureGenerationError(
            f"unsupported frontend API method {method!r} in {source_label}"
        )
    return method


def extract_frontend_route_contract(source_root: Path = FRONTEND_SOURCE_ROOT) -> set[str]:
    sources = {path: path.read_text(encoding="utf-8") for path in _frontend_source_files(source_root)}
    literals: dict[Path, list[dict[str, Any]]] = {}
    constants: dict[str, tuple[str, Path, int]] = {}
    for path, source in sources.items():
        literals[path] = [
            {"value": match.group("value"), "start": match.start(), "end": match.end()}
            for match in API_LITERAL_RE.finditer(source)
        ]
        for match in API_CONSTANT_RE.finditer(source):
            name = match.group("name")
            value = match.group("value")
            literal_start = match.start("quote")
            previous = constants.get(name)
            if previous and previous[0] != value:
                raise FixtureGenerationError(f"conflicting frontend API constant {name!r}")
            constants[name] = (value, path, literal_start)

    consumed: set[tuple[Path, int]] = set()
    routes: set[str] = set()

    def consume_literal(path: Path, start: int, end: int) -> None:
        for literal in literals[path]:
            if start <= literal["start"] and literal["end"] <= end:
                consumed.add((path, literal["start"]))

    for path, source in sources.items():
        for call_match in API_CALL_RE.finditer(source):
            parsed = _balanced_call_body(source, call_match.end() - 1)
            if parsed is None:
                raise FixtureGenerationError(
                    f"unterminated {call_match.group('name')} call in {path.relative_to(source_root)}"
                )
            body, body_start = parsed
            arguments = _split_call_arguments(body)
            endpoint_index = 1 if call_match.group("name") == "postSettingsForm" else 0
            if len(arguments) <= endpoint_index:
                continue
            endpoint_argument, argument_start, argument_end = arguments[endpoint_index]
            stripped_endpoint = endpoint_argument.strip()
            literal_match = API_LITERAL_RE.fullmatch(stripped_endpoint)
            endpoint: str | None = None
            if literal_match:
                endpoint = literal_match.group("value")
                consume_literal(
                    path,
                    body_start + argument_start,
                    body_start + argument_end,
                )
            elif re.fullmatch(r"[A-Za-z_$][\w$]*", stripped_endpoint):
                constant = constants.get(stripped_endpoint)
                if constant:
                    endpoint, constant_path, literal_start = constant
                    consumed.add((constant_path, literal_start))
            if endpoint is None or endpoint == "/api/":
                continue

            method = (
                "POST"
                if call_match.group("name") == "postSettingsForm"
                else _fetch_method(arguments, str(path.relative_to(source_root)))
            )
            routes.add(f"{method} {_normalized_api_path(endpoint)}")

        for return_match in RETURN_API_RE.finditer(source):
            endpoint = return_match.group("value")
            if endpoint == "/api/":
                continue
            consume_literal(path, return_match.start("quote"), return_match.end())
            routes.add(f"GET {_normalized_api_path(endpoint)}")

    unclassified = []
    for path, path_literals in literals.items():
        source = sources[path]
        for literal in path_literals:
            key = (path, literal["start"])
            if literal["value"] == "/api/" or key in consumed:
                continue
            line = source.count("\n", 0, literal["start"]) + 1
            unclassified.append(
                f"{path.relative_to(source_root)}:{line}: {literal['value']}"
            )
    if unclassified:
        raise FixtureGenerationError(
            "unclassified frontend API literals (use fetchWithTimeout/postSettingsForm or an explicit GET return):\n"
            + "\n".join(unclassified)
        )
    if not routes:
        raise FixtureGenerationError("frontend API route contract is empty")
    return routes


def fixture_route_contract(fixture: dict[str, Any]) -> set[str]:
    validate_fixture(fixture)
    return {
        route
        for scenario in fixture["scenarios"].values()
        for route in scenario
    }


def extract_registered_api_routes(routes_path: Path = WIFI_ROUTES_PATH) -> set[str]:
    source = routes_path.read_text(encoding="utf-8")
    routes = {
        f"{match.group('method')} {match.group('path')}"
        for match in REGISTERED_ROUTE_RE.finditer(source)
    }
    if not routes:
        raise FixtureGenerationError(f"no registered API routes found in {routes_path}")
    return routes


def validate_frontend_route_coverage(
    fixture: dict[str, Any],
    source_root: Path = FRONTEND_SOURCE_ROOT,
    routes_path: Path | None = WIFI_ROUTES_PATH,
) -> set[str]:
    frontend_routes = extract_frontend_route_contract(source_root)
    captured_routes = fixture_route_contract(fixture)
    missing = sorted(frontend_routes - captured_routes)
    extra = sorted(captured_routes - frontend_routes)
    if missing or extra:
        details = []
        if missing:
            details.append("missing frontend routes:\n  " + "\n  ".join(missing))
        if extra:
            details.append("non-frontend captured routes:\n  " + "\n  ".join(extra))
        raise FixtureGenerationError("fixture route contract mismatch; " + "\n".join(details))
    if routes_path is not None:
        unregistered = sorted(frontend_routes - extract_registered_api_routes(routes_path))
        if unregistered:
            raise FixtureGenerationError(
                "frontend fixture routes missing from production registration:\n  "
                + "\n  ".join(unregistered)
            )
    return frontend_routes


def run_native_emitter(pio: str) -> dict[str, Any]:
    emitted_fixtures = []
    with tempfile.TemporaryDirectory(prefix="v1-wifi-api-fixture-") as temp_dir:
        for suite in EMITTER_SUITES:
            command = [pio, "test", "-e", "native", "-f", suite]
            emitted_path = Path(temp_dir) / f"{suite}.json"
            environment = os.environ.copy()
            environment["V1_WIFI_API_FIXTURE_OUTPUT"] = str(emitted_path)
            environment["PLATFORMIO_BUILD_DIR"] = str(Path(temp_dir) / f"pio-build-{suite}")
            completed = subprocess.run(
                command,
                cwd=ROOT,
                env=environment,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            if completed.returncode != 0:
                raise FixtureGenerationError(
                    f"native fixture emitter {suite} failed with exit code "
                    f"{completed.returncode}\n{completed.stdout}"
                )
            if not emitted_path.is_file():
                raise FixtureGenerationError(
                    f"native fixture emitter {suite} did not create its requested output file"
                )
            try:
                emitted_fixtures.append(json.loads(emitted_path.read_text(encoding="utf-8")))
            except json.JSONDecodeError as error:
                raise FixtureGenerationError(
                    f"native fixture emitter {suite} returned invalid JSON: {error}"
                ) from error
    return merge_fixtures(emitted_fixtures)


def check_fixture(path: Path, generated_text: str) -> bool:
    current_text = path.read_text(encoding="utf-8") if path.exists() else ""
    if current_text == generated_text:
        return True

    diff = difflib.unified_diff(
        current_text.splitlines(keepends=True),
        generated_text.splitlines(keepends=True),
        fromfile=str(path),
        tofile=f"{path} (generated)",
    )
    sys.stderr.writelines(diff)
    return False


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail when the committed fixture differs instead of rewriting it",
    )
    parser.add_argument(
        "--pio",
        default=os.environ.get("PIO_CMD", "pio"),
        help="PlatformIO command (default: PIO_CMD or pio)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        fixture = run_native_emitter(args.pio)
        validate_frontend_route_coverage(fixture)
        rendered = canonical_fixture_text(fixture)
    except (FixtureGenerationError, OSError) as error:
        print(f"wifi API fixture generation failed: {error}", file=sys.stderr)
        return 1

    if args.check:
        if not check_fixture(FIXTURE_PATH, rendered):
            print(
                "wifi API fixture drift detected; run scripts/generate_wifi_api_fixtures.py",
                file=sys.stderr,
            )
            return 1
        print(f"wifi API fixture is current: {FIXTURE_PATH.relative_to(ROOT)}")
        return 0

    FIXTURE_PATH.parent.mkdir(parents=True, exist_ok=True)
    FIXTURE_PATH.write_text(rendered, encoding="utf-8")
    print(f"wrote {FIXTURE_PATH.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
