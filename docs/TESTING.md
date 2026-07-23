# Testing & CI

The repo's test infrastructure has three maintained shell entry points. They delegate to `pio`, the bench runner, and a fleet of Python gates under `scripts/`. The Python gates are the load-bearing part — they encode invariants that compile-time checks and unit tests can't catch on their own.

## Entry points

| Script | When | Scope |
|---|---|---|
| `./bench.sh` | Bench hardware evidence | Runs core and display SD/serial metric windows only. No OBD/proxy coverage or release-qualification language; optional promoted baselines are local comparison aids. |
| `scripts/ci-test.sh` | Every public `main` candidate / local pre-push | Authoritative repo gate. 40+ run-step gates across semantic checks, contracts, perf, frontend, and firmware build. A failed push-origin run blocks release. |
| `scripts/build_production_artifacts.sh` | Final stage of full CI and focused Release | Builds/deploys the frontend, then clean-builds and validates the production firmware and LittleFS artifacts exactly once. It assumes frontend dependencies are already installed. |

`scripts/ci-test.sh` remains the only authoritative approval gate and emits
`.artifacts/test_reports/ci-test/timing.json` for budget enforcement. The
production-artifact helper is a shared build stage, not a substitute for the
full authoritative main gate.

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

- `npm ci` — install the exact lockfile dependency graph; failures do not fall
  back to a mutable install.
- `npm run lint` — lint + type checks.
- `npm run test:coverage` — frontend unit tests with coverage.
- `npm run build` — production build, run by the shared production-artifact helper after the full frontend tests pass.
- `npm run deploy:built` — copy that already-built `interface/build` tree to `data/` for LittleFS packaging.
- `npm run deploy` — build first, then copy artifacts to `data/` for manual LittleFS packaging.

### Shared Production Artifact Build

- `build_production_artifacts.sh` is called by both full CI and Release so the
  published binaries follow the same production build path.
- `check_web_asset_budget.py` — total web asset size cap.
- `check_audio_asset_manifest.py` — source and deployed audio clips must match.

### Firmware Build

- `pio-check.sh` — `cppcheck` static analysis in the full CI gate only.
- The shared helper runs `pio run -t clean`, builds the `waveshare-349`
  firmware, checks memory headroom, builds LittleFS, validates package sizes,
  and mounts/lists the candidate LittleFS image.

`waveshare-349` is both the production firmware environment and the firmware
environment validated by CI/release. Do not introduce a lean release-only env:
if production build flags change, the CI build, release build, and bench upload
path must move together.

### Additional gates (run interleaved with the sections above)

Not itemized section-by-section to keep this walkthrough readable; all fail CI:
`check_modified_font_names.py`, `test_check_littlefs_image_compatibility.py`,
`test_run_device_tests_script.py`, `test_obd_proxy_qualification.py`,
`test_release_evidence_manifest.py`, `test_prepare_release_evidence_manifest.py`,
`test_release_workflow_flash_contract.py`,
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

## Firmware C++ coverage lane (out-of-band)

Coverage of the firmware C++ is measured **outside the authoritative main gate**, by
`.github/workflows/coverage.yml` on a weekly schedule and on
`workflow_dispatch`.

### What the number actually means — read before quoting it

The native environments set `test_build_src = false`. A native suite compiles
only the production units and mocks it `#include`s, not the whole firmware
image. So this lane measures **coverage of the firmware code the native suites
actually build** — the meaningful denominator for host-testable logic. It is
**not** whole-firmware coverage and must never be reported as such.

Device-only driver code (Arduino / NimBLE / display-driver bound paths) is
outside this denominator and remains covered by the critical mutation gate and
the device suites, exactly as before. Nothing about that changed.

Every run writes an explicit measured-scope file list — the complete
denominator, file by file — to
`.artifacts/test_reports/coverage/coverage_scope.txt`, so the headline
percentage can always be audited against the files it was computed over.

### Pieces

| Piece | Role |
|---|---|
| `[env:native-coverage]` in `platformio.ini` | Clone of `[env:native]` plus `--coverage -fprofile-abs-path -O0 -g` (compile) and `-lgcov` (link). PlatformIO routes `build_flags` through SCons `ParseFlags`, which sends bare flags to `CCFLAGS` but only `-l` flags to `LIBS`, so both halves must be named explicitly — dropping `-lgcov` breaks the link. |
| `run_native_tests_serial.py --env native-coverage` | Runs each suite in its own `PLATFORMIO_BUILD_DIR`, so `.gcno`/`.gcda` stay isolated per suite and never collide. |
| `run_firmware_coverage.py` | Runs the instrumented suites, aggregates with gcovr filtered to `src/` + `include/`, emits JSON + HTML + the scope list into `.artifacts/test_reports/coverage/`. `--write-baseline` refreshes the tracked baseline (full-suite runs only). |
| `test/contracts/coverage_baseline.json` | Tracked ratchet baseline: overall line % plus per-file line %. |
| `check_firmware_coverage.py` | The ratchet. Fails if overall — or any tracked file — drops more than `tolerance_pp` (0.5) below baseline. Prints a suggested baseline bump when coverage rises. |
| `test_check_firmware_coverage.py` | Regression suite for the ratchet, including synthetic coverage-drop cases that prove the checker fails. |

Excluded from the denominator: tests and mocks (`test/`), library deps
(`.pio/`), and generated data headers (`v1simple_logo.h`, `warning_audio.h`,
`Segment7Font.h`, `FreeSans*.h`) — megabytes of constant arrays that would
dilute the ratchet into noise.

### Why it is not in the authoritative main gate

Measured on one host, over all 149 native suites:

| Lane | Wall clock |
|---|---|
| `run_native_tests_serial.py` (`native`, uninstrumented) | 170.7s |
| `run_native_tests_serial.py --env native-coverage` (instrumented) | 206.6s (1.21x) |
| gcovr aggregation over the 149 per-suite build dirs | ~24s |
| **Coverage lane total** | **~231s** |

The instrumented suite itself is only ~21% slower — but the gate does not
*replace* its native run with the instrumented one, it would have to add a whole
second run. The authoritative gate now also carries 192 native suites,
sanitizers, linked-source pilots, HIL qualification, and bound production
rebuilds. A cold GitHub run exceeded the former 25-minute workflow timeout after
1482 seconds, while the same committed gate passed locally in 1106 seconds.
The tracked 1800-second budget and 36-minute workflow timeout reserve 60 seconds
for job setup plus a five-minute cancellation margin. They are enforced as one
contract by `check_ci_budget.py`. Coverage remains out of band so that margin
is reserved for cold-run variance rather than a duplicate native lane.

### Running it locally

```bash
python3 scripts/run_firmware_coverage.py            # full suite -> JSON + HTML + scope list
python3 scripts/check_firmware_coverage.py          # ratchet against the tracked baseline
./scripts/ci-test.sh --with-coverage                # opt-in: gate, then the coverage lane
```

`./scripts/ci-test.sh` with no flag is unchanged and does not run coverage. With
`--with-coverage` the coverage section runs *after* the budget check, so its wall
clock is deliberately not charged against the 1200s `ci-test` budget — it is not
part of the authoritative gate.

The scheduled workflow fails when the ratchet drops, but it does not gate a PR,
merge, or release. Treat a failure as follow-up work rather than a reason to run
the full suite again during the delivery path.

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
- `tools/synthetic_maintenance_check.sh <device-ip>` (in `tools/`, not `scripts/`) — synthetic contract check against a live maintenance-mode unit: asserts the HTTP status codes and JSON keys the web UI consumes, the published maintenance deadline, diagnostics listing, live-push maintenance gating, every production page (including Logs), the `X-V1Simple-Request` write header, the static-path guard, and that the unit serves the repo's current UI build. Read-only except one benign display preview. Run it inside the 10-minute maintenance window (takes ~30 s).
- `check_obd_proxy_qualification.py` — validates a typed OBD/proxy hardware evidence pack against `tools/obd_proxy_qualification_profile_v1.json`; normal CI exercises only its synthetic regression suite.
- `check_bug_squash_hil_qualification.py` — validates a local, typed bug-squash HIL evidence pack against `tools/bug_squash_hil_qualification_profile_v1.json`; normal CI exercises its adversarial regression suite without operating hardware.
- `generate_bug_squash_build_evidence.py` — clean-HEAD-only generator for retained PlatformIO build manifests, logs, binaries, content-hashed tool identities, and ESP32-S3 image validation used by that evidence contract.

## Bug-squash HIL evidence contract

The versioned bug-squash profile covers exactly `BSC-02` through `BSC-14` plus
`BSC-16`. It is an evidence contract, not a scenario runner, and hardware is
not operated by the validator.

The current version 3 profile is explicitly `blocked`. Production and
car-production builds map to real PlatformIO environments, but the required
bounded HIL fault-control build is marked
`hil-fault-control-not-implemented`; it has no claimed environment or build
command. The case-driver contract is also blocked with
`case-source-provenance-not-authenticated` because no typed physical
driver/collector exists yet. Version 3 adds useful integrity binding for build
artifacts and private board inventory, but neither is authenticated: the build
flow has no pinned/trusted PlatformIO or rebuild root, and the board flow has no
external signing key or pinned inventory trust root. All four activation
requirements therefore remain blocked. Adding a made-up environment, editing
readiness flags, or recomputing self-declared evidence digests does not activate
the profile or authenticate old evidence.

Each local evidence pack must bind its result to the caller-supplied full target
commit and the repository's current, existing `HEAD`; the live worktree must be
clean. The validator derives the target commit time and `FIRMWARE_VERSION`
directly from that Git tree. Qualification, build, resolution, and case evidence
must be at or after the target commit. The validator pins the tracked profile by
SHA-256 and does not accept a custom profile.

Build evidence schema 3 commits every active build to the full target Git tree,
the exact pinned build contract, the target-tracked generator bytes, and a
content identity for the PlatformIO launcher/package, Python executable, Git
executable, and esptool package. Each retained log starts with the same exact
commit, tree, contract, tool, environment, and time identity. Input, output,
per-build, and whole-manifest domain-separated commitments bind the log and
binary digests to that context. The validator independently recomputes every
target-, profile-, tool-, log-, and artifact-derived value; merely rewriting
the unkeyed commitments cannot conceal a stale commit, changed tool, changed
contract, changed log, or changed binary.

These commitments provide mutation detection, not an origin certificate. An
evidence author who controls the selected PlatformIO executable and stale build
directory can author a mutually consistent pack. Build provenance must remain
blocked until validation is rooted in a trusted tool identity or independent
rebuild service.

The validator also runs `esptool image-info` against every retained binary.
Each must be a complete ESP32-S3 application image with valid checksum and
validation hash and embed the firmware-version identity derived from the
target Git tree. The generator rejects output outside ignored `.artifacts/`,
missing active PlatformIO environments, dirty source, failed builds, and
invalid images. A blocked build contract is never executed or silently treated
as optional qualification evidence.

Every selected DUT and rig has a salted inventory commitment and a sanitized
resolver attestation. The attestation exposes only the alias, selected
capabilities, resolution digest, inventory commitment, schemas, algorithm, and
observation time; it never embeds the salt, configured USB identity, serial
path, LAN endpoint, or unrelated inventory metadata. The ignored local binding
retains the salt, selected private inventory record, and exact resolution so
the validator can independently recompute both public digests. Any change to
the selected inventory bytes, salt, or resolution invalidates the attestation.
For dynamic LAN, the resolver opens the exact selected serial port and performs
bounded collection; it does not accept a caller-provided serial log.
Because the evidence author still supplies the local salt, inventory record,
and resolution, this is also integrity binding rather than authentication.
Board provenance must remain blocked until an external signature or pinned
private inventory root is verified.

Case runs bind an approved DUT and rig to case-specific structured roles. Those
roles enumerate the exact stimuli, bounded fault lifecycle,
barrier-ready/release markers, VBUS-isolation fact, expected/unexpected reset
contract, instrumentation mode, and typed acceptance facts. Every observation
also references a separate source artifact whose exact record set, values,
timestamps, and causal ordering are validated. Its commitment binds the exact
pinned case definition, driver contract, and target-tracked
execution-provenance manifest. That manifest digests the runner, future driver
sources, and future fault-control implementation/tests exactly as stored in the
target commit. Roles with a fault or barrier cannot claim manual-only
instrumentation. HIL-fault cases require separate production-replay evidence.
Opaque or reused evidence cannot close multiple roles, and `ACCEPTED_RISK`
cannot close the pack or any case. The source schema is ready to bind tracked
drivers, but the current driver and fault-control path lists remain empty and
blocked, so authored source records still cannot close qualification.

All referenced artifacts must be unique, nonempty regular files below the pack
directory. Every path component is lstat-checked, symlinks and operationally
identifying path shapes are rejected, and each file's SHA-256 is verified.
Qualification and nested observation timestamps must be UTC, ordered, and not
in the future. Unexpected panic, watchdog, and per-case reset counts must be
integer zero.

Generate the implemented build and execution-provenance evidence below the
ignored artifact root with the first command. Once all pinned infrastructure
is active, validate a completed local pack with the second:

```bash
python3 scripts/generate_bug_squash_build_evidence.py \
  --output-directory .artifacts/bug-squash-hil/<run-id>

python3 scripts/check_bug_squash_hil_qualification.py \
  --artifact .artifacts/bug-squash-hil/<run-id>/qualification_result.json \
  --expected-git-sha <40-hex-target-sha>
```

With the current blocked profile, the generator builds and retains only the
active production and car-production contracts. It records but never executes
the blocked `hil-fault` contract. The qualification validator rejects every
claimed PASS pack until the typed case driver and bounded fault control are
implemented, trusted build and board provenance roots are verified, and all
four pinned activation requirements are deliberately activated.

### Final-device wrapper

The separate final-device wrapper runs the existing device-unit suite through
an exact local inventory alias:

```bash
python3 scripts/run_bug_squash_hil.py --run-device-suite --board release
```

This mode requires `HEAD` and the whole worktree, including untracked files, to
be clean before execution and rechecks that state after hardware commands. The
resolver exact-matches the requested `release` alias and configured USB serial;
it always uses live enumeration and never accepts a caller-supplied port list or
falls back to a sole, first, or different detected port. Authoritative mode pins
the tracked runner paths, the default isolated PlatformIO environment, and a
sanitized command path; test overrides are explicitly non-authoritative and can
only report `TEST_PASS`. The device runner is invoked in fail-closed transport
mode: a nonzero command exit, transport failure, non-PASS suite row, or positive
PlatformIO `error_nums` rejects the run even if another layer exits zero. JSON,
JUnit XML, scoring, manifest, and suite-index identities and counts must agree.

The wrapper stores the private inventory/resolution binding only below the
ignored raw artifact directory and publishes the sanitized salted commitment
attestation beside the retained run. It hashes the complete retained device
artifact tree plus its command and resolver evidence, records the full target
SHA, and restores the pinned `waveshare-349` production image before reporting
success. Restoration failure
rejects the run. All `--case BSC-*` modes intentionally return
`case_driver_unavailable`; none can run or claim PASS until its typed physical
driver and provenance contract are implemented.

Keep the pack under ignored `.artifacts/`. Use stable aliases in the result;
never add lab-specific serial numbers, device paths, IP addresses, credentials,
or captured operational logs to tracked files. Reserved documentation examples
are not lab identity. The checked-in profile and synthetic tests contain no
lab-specific values.

## Where new gates go

When you add a new contract or invariant:

1. Write the gate as `scripts/check_<name>.py`. The script must exit non-zero on violation and print the offending file:line. Carry a docstring describing the rule.
2. Add it to `scripts/ci-test.sh` under the appropriate `section`. Order matters — gates that read source go before gates that depend on a built artifact.
3. If the rule has an allowlist, put the allowlist file under `test/contracts/` (one path or `file:line` per line).
4. If the gate has its own meta-tests (the `test_*.py` pattern for `check_*.py`), wire those into `ci-test.sh` adjacent to the gate.

## Notes for maintainers

`run_step` aborts the lane on failure (`set -e`). Don't add steps that "warn but continue" — if a check is advisory, it doesn't belong in the gate.

Section ordering in `ci-test.sh` is intentional: cheap semantic gates first (fast failure for grep-style violations), then expensive things (compile, frontend build, size report). If you reorder, you waste developer time waiting for slow lanes to fail on a checkable problem.

The mutation gate (`--critical` lane) runs the maintained curated catalog. Don't
add mutations to that catalog without verifying they're stable — a flaky
critical mutation blocks every authoritative CI run.
