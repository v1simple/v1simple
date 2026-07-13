# Testing & CI

The repo's test infrastructure has two maintained shell entry points. They delegate to `pio`, the bench runner, and a fleet of Python gates under `scripts/`. The Python gates are the load-bearing part — they encode invariants that compile-time checks and unit tests can't catch on their own.

## Entry points

| Script | When | Scope |
|---|---|---|
| `./bench.sh` | Bench hardware evidence | Runs core and display SD/serial metric windows only. No OBD/proxy coverage or release-qualification language; optional promoted baselines are local comparison aids. |
| `scripts/ci-test.sh` | Every PR / local pre-push | Authoritative repo gate. 40+ run-step gates across semantic checks, contracts, perf, frontend, and firmware build. Fail = block merge. |

`scripts/ci-test.sh` is the only maintained CI lane; it emits `.artifacts/test_reports/ci-test/timing.json` for budget enforcement.

## `scripts/ci-test.sh` walkthrough

Sections run in this order. A failing step aborts the run (`set -euo pipefail`).

### Semantic Gates

Pattern-match scans of source for known-bad shapes. Most are read-only — they don't modify code, they just reject patterns.

- `check_bug_patterns.py` — scans firmware source for known bug patterns; `test_bug_pattern_scanner.py` is its own regression suite.
- `check_sdkconfig_redefines.py` — rejects inert `-D CONFIG_*` flags in `platformio.ini` when the framework's prebuilt `sdkconfig.h` defines the same symbol.
- `check_ble_deletion_contract.py` — forbids deleting `V1BLEClient` at runtime (NimBLE has a 3-slot internal array that corrupts on client deletion).
- `check_frontend_http_resilience_contract.py` — frontend must handle HTTP failures gracefully.
- `check_ble_hot_path_semantic_guard.py` — guards against allocations / heavy work in BLE notify path.
- `check_display_flush_semantic_guard.py` — display flush must not block.
- `check_main_loop_semantic_guard.py` — main-loop body must not violate phase ordering.
- `check_module_const_correctness.py` — public methods that don't mutate must be `const`.
- `check_extern_escape.py` — forbids `extern` declarations escaping their owning translation unit.
- `check_header_style_contract.py` — production headers must use `#pragma once`; the only remaining include guards are explicitly tracked native-test mock-shadow seams.
- `check_retired_alp_terms.py` — forbids retired ALP working-mode vocabulary (`scan`, `armed`, `detection`, `defense`; current vocabulary is PDC / DLI / LID per the AL Priority manual).
- `check_stabilization_manifest.py` — stabilization manifest contract.
- `run_native_tests_serial.py` — runs `pio test -e native` (the unit-test suite, ~1500 tests).
- `run_functional_tests.sh` — functional scenarios against canned packets.

### Critical Mutation Gate

- `mutation_test.sh --critical` — runs the tracked critical mutation catalog (mutates source, asserts tests catch it).

### Perf Scoring Gate

Regression tests for the perf-metrics scoring + schema toolchain itself, not for the device's perf.

- `test_perf_scoring.py` — deterministic perf scorer regression tests.
- `check_perf_computed_metric_contract.py` — every computed metric must have schema entry.
- `test_metric_schema.py` — shared metric schema regression tests.
- `test_hardware_run_scoring.py`, `test_perf_csv_import.py`, `test_soak_parse_metrics.py` — toolchain regression suites.
- `test_bench_score.py` — bench result regression suite.

### Compatibility Guards

Contracts that pin specific patterns. Failing one of these is the most common reason a PR gets blocked.

- `check_wifi_api_contract.py` — WiFi API contract (route registrations, response shape).
- `check_reorder_warning_contract.py` — reorder-warning enforcement (member-init ordering).
- `check_quiet_coordinator_contract.py` — only `QuietCoordinatorModule` may call `V1BLEClient::setMute()`/`setVolume()` directly. Production code outside the coordinator must route through it.
- `check_connection_cycle_invariants.py` — V1/OBD/Proxy connection ordering.
- `check_ble_hot_path_contract.py` — BLE hot-path snapshot contract.
- `check_perf_csv_column_contract.py` — perf CSV column ordering pinned across firmware versions.
- `check_perf_schema_contract.py` — perf schema referential integrity.
- `check_display_flush_discipline_contract.py` — display flush rules.
- `check_dirty_flag_discipline.py` — every `display_*.cpp` function that reads a `dirty.xxx` flag must also clear it in the same function body. Two failure modes result if you don't: stale dirty flags trigger redraws forever, or cleared flags miss a real change.
- `check_main_loop_call_order_contract.py` — main-loop phase ordering.
- `check_obd_boot_safety_contract.py` — OBD must not start before V1 boot dwell.
- `check_extern_usage_contract.py` — `extern` symbols allowlist (`test/contracts/extern_usage_contract.txt`).
- `check_std_function_usage_contract.py` — forbids `std::function` in production source. Allowlist at `test/contracts/std_function_allowlist.txt` (one `file:line` per entry).

### Docs Hygiene

- `check_perf_slo_contract.py` — validates `docs/PERF_SLOS.md` tables against `tools/perf_slo_thresholds.json`.
- `check_build_instruction_contract.py` — README/build-instruction sanity.
- `check_v1_protocol_docs_contract.py` — V1 protocol comments/docs must cite tracked `docs/V1_PROTOCOL_REFERENCES.md` instead of local scratch PDFs.
- `check_protocol_spec_tables.py` — the machine-readable spec tables in `docs/V1_PROTOCOL_REFERENCES.md` are the single source of truth for protocol decode tables; regenerates and verifies `test/fixtures/protocol_spec_tables.h`, which `test_protocol_spec_conformance` drives through the real parse path. Change a table with `--write`; the conformance suite arbitrates doc vs firmware.

### Frontend

`cd interface` then `npm`-driven steps:

- `npm ci` (or `npm install` fallback) — install deps.
- `npm run lint` — lint + type checks.
- `npm run test:coverage` — frontend unit tests with coverage.
- `npm run build` — production build.
- `npm run deploy:built` — copy an already-built `interface/build` tree to `data/` for LittleFS packaging.
- `npm run deploy` — build first, then copy artifacts to `data/` for manual LittleFS packaging.

### Frontend Packaging

- `check_web_asset_budget.py` — total web asset size cap.

### Firmware Build

- `pio-check.sh` — `cppcheck` static analysis.
- `pio run -t clean` — fresh build.
- `pio run` — firmware build (`waveshare-349` env by default).
- `pio run -t buildfs` — LittleFS image.
- `report_flash_package_size.py` — verifies firmware ≤ 5,570,560 bytes and LittleFS == 2,424,832 bytes. Mismatch fails CI.

`waveshare-349` is both the production firmware environment and the firmware
environment validated by CI/release. Do not introduce a lean release-only env:
if production build flags change, the CI build, release build, and bench upload
path must move together.

### Additional gates (run interleaved with the sections above)

Not itemized section-by-section to keep this walkthrough readable; all fail CI:
`check_modified_font_names.py`, `test_check_littlefs_image_compatibility.py`,
`test_run_device_tests_script.py`, `test_release_evidence_manifest.py`,
`check_littlefs_mount_contract.py`, `check_build_dist_contract.py`,
`check_release_workflow_flash_contract.py`, `check_web_installer_page.py`,
`check_api_doc_sources.py`, `check_alp_protocol_docs_contract.py`,
`check_audio_asset_manifest.py`, `check_memory_headroom.py`,
`check_littlefs_image_compatibility.py`.

### Budget Check

- `check_ci_budget.py ci-test <timing.json>` — total wall-clock budget for the lane.

### Size Report

`pio run -t size` — informational; not a gate.

## Python script reference

Located in `scripts/`. Naming convention:

- `check_*.py` — gate that fails CI on contract violation.
- `test_*.py` — regression test for the toolchain itself (the gate code).
- `run_*.py` / `run_*.sh` — executes a test workflow.
- `analyze_*.py`, `*_capture.py`, `verify_*.py`, `validate_*.py` — analysis / verification helpers.
- `*_metric*.py`, `*scoring*.py` — perf scoring toolchain.

To find what a specific gate enforces: `head -10 scripts/<name>.py` — every gate carries a docstring with the rule.

## Native vs device unit-test scope

`pio test -e native` is the authoritative unit-test lane and includes the
mock-backed suites. The `device` environment intentionally ignores those
native-only suites in `platformio.ini` (`test_ignore`) because they depend on
host mocks for Arduino, WebServer, Serial, and module shims; it covers only the
device-focused suites plus a small set of self-contained shared suites.

## Bench hardware evidence (`./bench.sh`)

`./bench.sh` is the only maintained top-level hardware evidence command. It
answers the bench question only: are the production-image core runtime and
display pipeline healthy while metrics are collected to SD?

Default live scope:

1. `bench_core` — starts one firmware core window through the serial Q* protocol.
2. `bench_display` — starts one firmware display-preview window through the same serial path.
3. `tools/bench_score.py` — reduces the imported artifacts to one bench result.

The bench path deliberately does **not** run OBD-vs-proxy modes, physical
coverage gates, certified baselines, moving baselines, or device-unit suites.
It can compare against a locally promoted single-run baseline for variance
visibility, but that baseline is not a qualification verdict. The broader
concerns were removed from the bench signal because they made lab results
ambiguous and did not answer the core/display bench question.

Examples:

```bash
./bench.sh
./bench.sh --core
./bench.sh --display
./bench.sh --duration-seconds 600
./bench.sh --from-csv /path/to/perf_boot_1.csv --display
./bench.sh --promote-baseline      # save a PASS as the local comparison baseline
./bench.sh --no-baseline           # score without local baseline comparison
```

Artifacts land under `.artifacts/bench/<board-id>/runs/<timestamp>_<sha>/` with
`core/`, `display/`, `bench_result.json`, and `bench_summary.txt`.

Bench result language is intentionally small:

- `PASS` — collection succeeded and no hard/advisory failures were reported.
- `WARN` — collection succeeded with advisory warnings.
- `FAIL` — collection succeeded and at least one hard metric/SLO failed.
- `COLLECTION_FAILED` — CSV, import, scoring, or serial collection evidence is missing/invalid.

## Other scripts of note

- `mutation_test.py` / `mutation_test.sh` — mutation testing harness; `--critical` runs the tracked catalog.
- `analyze_alp_fingerprints.py` — ALP frame fingerprint analysis.
- `pio-size.sh` — size report wrapper.
- `tools/synthetic_maintenance_check.sh <device-ip>` (in `tools/`, not `scripts/`) — synthetic contract check against a live maintenance-mode unit: asserts the HTTP status codes and JSON keys the web UI consumes, maintenance 409 gating, the `X-V1Simple-Request` write header, the static-path guard, and that the unit serves the repo's current UI build. Read-only except one benign display preview. Run it inside the 10-minute maintenance window (takes ~30 s).


## Where new gates go

When you add a new contract or invariant:

1. Write the gate as `scripts/check_<name>.py`. The script must exit non-zero on violation and print the offending file:line. Carry a docstring describing the rule.
2. Add it to `scripts/ci-test.sh` under the appropriate `section`. Order matters — gates that read source go before gates that depend on a built artifact.
3. If the rule has an allowlist, put the allowlist file under `test/contracts/` (one path or `file:line` per line).
4. If the gate has its own meta-tests (the `test_*.py` pattern for `check_*.py`), wire those into `ci-test.sh` adjacent to the gate.

## Notes for maintainers

`run_step` aborts the lane on failure (`set -e`). Don't add steps that "warn but continue" — if a check is advisory, it doesn't belong in the gate.

Section ordering in `ci-test.sh` is intentional: cheap semantic gates first (fast failure for grep-style violations), then expensive things (compile, frontend build, size report). If you reorder, you waste developer time waiting for slow lanes to fail on a checkable problem.

The mutation gate (`--critical` lane) runs the maintained curated catalog. Don't add mutations to that catalog without verifying they're stable — a flaky critical mutation blocks every PR.
