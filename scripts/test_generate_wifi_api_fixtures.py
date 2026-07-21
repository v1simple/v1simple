#!/usr/bin/env python3

from __future__ import annotations

from contextlib import redirect_stderr
import io
import json
from pathlib import Path
import tempfile
import unittest

import generate_wifi_api_fixtures as generator


def sample_fixture() -> dict:
    return {
        "schemaVersion": 1,
        "scenarios": {
            "sample": {
                "GET /api/example": [
                    {
                        "status": 200,
                        "contentType": "application/json",
                        "body": {"z": 2, "a": 1},
                    }
                ]
            }
        },
    }


class GenerateWifiApiFixturesTest(unittest.TestCase):
    def test_extracts_marker_and_canonicalizes_nested_keys(self) -> None:
        payload = json.dumps(sample_fixture(), separators=(",", ":"))
        fixture = generator.extract_fixture(f"noise\n{generator.FIXTURE_MARKER}{payload}\nmore noise")

        rendered = generator.canonical_fixture_text(fixture)

        self.assertLess(rendered.index('"a": 1'), rendered.index('"z": 2'))
        self.assertTrue(rendered.endswith("\n"))
        self.assertEqual(rendered, generator.canonical_fixture_text(fixture))

    def test_rejects_missing_or_duplicate_emitter_records(self) -> None:
        with self.assertRaises(generator.FixtureGenerationError):
            generator.extract_fixture("no fixture here")

        payload = json.dumps(sample_fixture())
        output = f"{generator.FIXTURE_MARKER}{payload}\n{generator.FIXTURE_MARKER}{payload}"
        with self.assertRaises(generator.FixtureGenerationError):
            generator.extract_fixture(output)

    def test_rejects_invalid_response_contract(self) -> None:
        fixture = sample_fixture()
        fixture["scenarios"]["sample"]["GET /api/example"][0]["status"] = 99

        with self.assertRaises(generator.FixtureGenerationError):
            generator.validate_fixture(fixture)

        fixture = sample_fixture()
        fixture["scenarios"]["sample"]["GET /api/example?query=1"] = fixture[
            "scenarios"
        ]["sample"].pop("GET /api/example")
        with self.assertRaises(generator.FixtureGenerationError):
            generator.validate_fixture(fixture)

    def test_normalizes_flat_emitter_records_into_ordered_route_sequences(self) -> None:
        emitted = {
            "schemaVersion": 1,
            "captures": [
                {
                    "scenario": "scan",
                    "route": "GET /api/wifi/scan",
                    "status": 200,
                    "contentType": "application/json",
                    "body": {"scanning": True},
                },
                {
                    "scenario": "scan",
                    "route": "GET /api/wifi/scan",
                    "status": 200,
                    "contentType": "application/json",
                    "body": {"scanning": False},
                },
            ],
        }

        fixture = generator.normalize_emitter_fixture(emitted)

        responses = fixture["scenarios"]["scan"]["GET /api/wifi/scan"]
        self.assertEqual([True, False], [response["body"]["scanning"] for response in responses])

    def test_check_fixture_detects_drift(self) -> None:
        generated = generator.canonical_fixture_text(sample_fixture())
        with tempfile.TemporaryDirectory() as temp_dir:
            fixture_path = Path(temp_dir) / "fixture.json"
            fixture_path.write_text(generated, encoding="utf-8")
            self.assertTrue(generator.check_fixture(fixture_path, generated))

            fixture_path.write_text("{}\n", encoding="utf-8")
            with redirect_stderr(io.StringIO()):
                self.assertFalse(generator.check_fixture(fixture_path, generated))

    def test_merges_disjoint_scenarios_and_rejects_collisions(self) -> None:
        first = sample_fixture()
        second = {
            "schemaVersion": 1,
            "scenarios": {
                "second": {
                    "POST /api/second": [
                        {
                            "status": 503,
                            "contentType": "application/json",
                            "body": {"error": "unavailable"},
                        }
                    ]
                }
            },
        }

        merged = generator.merge_fixtures([first, second])
        self.assertEqual({"sample", "second"}, set(merged["scenarios"]))

        with self.assertRaises(generator.FixtureGenerationError):
            generator.merge_fixtures([first, first])

    def test_extracts_query_normalized_frontend_route_contract(self) -> None:
        source = """
const PROFILE_ENDPOINT = '/api/profile?source=fixture';
await fetchWithTimeout(PROFILE_ENDPOINT);
await fetchWithTimeout(`/api/profile?name=${name}`, { method: 'POST' });
await postSettingsForm(formData, '/api/settings');
function download(path) { return `/api/log?path=${path}`; }
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            source_root = Path(temp_dir)
            (source_root / "app.svelte").write_text(source, encoding="utf-8")
            (source_root / "api.ts").write_text(
                "fetchWithTimeout('/api/typescript', "
                "{ body: JSON.stringify({ method: 'DELETE' }) });\n",
                encoding="utf-8",
            )
            (source_root / "component.tsx").write_text(
                "fetchWithTimeout('/api/component', { method: 'PATCH' });\n",
                encoding="utf-8",
            )

            routes = generator.extract_frontend_route_contract(source_root)

        self.assertEqual(
            {
                "GET /api/profile",
                "POST /api/profile",
                "POST /api/settings",
                "GET /api/log",
                "GET /api/typescript",
                "PATCH /api/component",
            },
            routes,
        )

    def test_frontend_route_extraction_fails_on_unclassified_api_literal(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            source_root = Path(temp_dir)
            (source_root / "app.js").write_text(
                "const UNUSED_ENDPOINT = '/api/unclassified';\n", encoding="utf-8"
            )

            with self.assertRaisesRegex(
                generator.FixtureGenerationError, "unclassified frontend API literals"
            ):
                generator.extract_frontend_route_contract(source_root)

    def test_frontend_coverage_rejects_missing_and_extra_routes(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            source_root = Path(temp_dir)
            (source_root / "app.js").write_text(
                "fetchWithTimeout('/api/example');\n", encoding="utf-8"
            )
            self.assertEqual(
                {"GET /api/example"},
                generator.validate_frontend_route_coverage(
                    sample_fixture(), source_root, routes_path=None
                ),
            )

            missing = sample_fixture()
            missing["scenarios"]["sample"]["GET /api/other"] = missing["scenarios"][
                "sample"
            ].pop("GET /api/example")
            with self.assertRaisesRegex(
                generator.FixtureGenerationError, "missing frontend routes"
            ):
                generator.validate_frontend_route_coverage(missing, source_root, routes_path=None)

            extra = sample_fixture()
            extra["scenarios"]["sample"]["GET /api/extra"] = extra["scenarios"][
                "sample"
            ]["GET /api/example"]
            with self.assertRaisesRegex(
                generator.FixtureGenerationError, "non-frontend captured routes"
            ):
                generator.validate_frontend_route_coverage(extra, source_root, routes_path=None)

    def test_frontend_route_extraction_rejects_dynamic_or_unsupported_methods(self) -> None:
        for method_value in ("requestMethod", "'TRACE'"):
            with self.subTest(method_value=method_value), tempfile.TemporaryDirectory() as temp_dir:
                source_root = Path(temp_dir)
                (source_root / "app.ts").write_text(
                    "fetchWithTimeout('/api/example', "
                    f"{{ method: {method_value} }});\n",
                    encoding="utf-8",
                )
                with self.assertRaisesRegex(
                    generator.FixtureGenerationError,
                    "dynamic fetch method|unsupported frontend API method",
                ):
                    generator.extract_frontend_route_contract(source_root)

    def test_frontend_route_extraction_rejects_implicit_method_overrides(self) -> None:
        sources = {
            "method shorthand": (
                "const method = 'POST';\n"
                "fetchWithTimeout('/api/example', { method });\n"
            ),
            "computed method": (
                "fetchWithTimeout('/api/example', { ['method']: 'POST' });\n"
            ),
            "init spread": (
                "const postOptions = { method: 'POST' };\n"
                "fetchWithTimeout('/api/example', { ...postOptions });\n"
            ),
            "static method followed by spread": (
                "const overrides = { method: 'POST' };\n"
                "fetchWithTimeout('/api/example', { method: 'GET', ...overrides });\n"
            ),
        }
        for label, source in sources.items():
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temp_dir:
                source_root = Path(temp_dir)
                (source_root / "app.ts").write_text(source, encoding="utf-8")
                with self.assertRaisesRegex(
                    generator.FixtureGenerationError,
                    "dynamic fetch method|dynamic fetch init spread",
                ):
                    generator.extract_frontend_route_contract(source_root)

    def test_frontend_route_extraction_allows_unrelated_shorthand_init_fields(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            source_root = Path(temp_dir)
            (source_root / "app.ts").write_text(
                "const headers = {};\n"
                "fetchWithTimeout('/api/example', { headers });\n",
                encoding="utf-8",
            )

            self.assertEqual(
                {"GET /api/example"},
                generator.extract_frontend_route_contract(source_root),
            )

    def test_frontend_coverage_rejects_unregistered_route_labels(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            source_root = temp_root / "src"
            source_root.mkdir()
            (source_root / "app.js").write_text(
                "fetchWithTimeout('/api/example');\n", encoding="utf-8"
            )
            routes_path = temp_root / "wifi_routes.cpp"
            routes_path.write_text(
                'server_.on("/api/other", HTTP_GET, []() {});\n', encoding="utf-8"
            )

            with self.assertRaisesRegex(
                generator.FixtureGenerationError, "missing from production registration"
            ):
                generator.validate_frontend_route_coverage(
                    sample_fixture(), source_root, routes_path
                )

            routes_path.write_text(
                'server_.on("/api/example", HTTP_GET, []() {});\n', encoding="utf-8"
            )
            self.assertEqual(
                {"GET /api/example"},
                generator.validate_frontend_route_coverage(
                    sample_fixture(), source_root, routes_path
                ),
            )

    def test_production_frontend_contract_has_expected_exact_shape(self) -> None:
        routes = generator.extract_frontend_route_contract()

        self.assertEqual(52, len(routes))
        self.assertEqual(21, sum(route.startswith("GET ") for route in routes))
        self.assertEqual(31, sum(route.startswith("POST ") for route in routes))
        self.assertIn("GET /api/v1/profile", routes)
        self.assertIn("GET /api/diagnostics/log", routes)


if __name__ == "__main__":
    unittest.main()
