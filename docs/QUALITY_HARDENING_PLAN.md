# Quality Hardening Plan

## Purpose

Raise the repository's architecture, security, hardware-confidence, and
repository-hygiene quality without destabilizing the production firmware.
Every work package below is intentionally small, independently reviewable, and
reversible. A later package must not rely on an unverified earlier package.

## Protected baseline

- Branch point: `f5fba78` (`v1simple 1.0.1`).
- Working branch: `dev/quality-hardening`.
- Authoritative local CI: PASS in 538 seconds (timed gate: 531/1200 seconds).
- Firmware: 2,158,480 bytes; SHA-256
  `a817825f21b08666be83ac7572257d4b8a28575263c35b318c05f9d503e8e35f`.
- LittleFS: 2,424,832 bytes; SHA-256
  `07cc07ae0b8b4b4f893357bb5436466a24c9b8f505f740bddc01e9562b0a095a`.
- Memory use: flash 2,157,973 bytes, IRAM 103,680 bytes, RAM 80,240 bytes.
- Fresh hardware evidence for this exact snapshot is still required before any
  hardware-sensitive release decision. Older local bench artifacts are not a
  substitute because they belong to other snapshots and include disconnected
  peer failures.

## Non-negotiable safety rules

1. Keep refactoring separate from behavior changes.
2. Keep the old path available until a replacement has passed in parallel.
3. Do not move ownership or change static object lifetimes during dependency
   injection work.
4. Do not add heap allocation to BLE, display, parser, or main-loop hot paths.
5. Do not change settings schema, NVS keys, backup format, or credential
   encoding in this program.
6. Do not enable NVS encryption, flash encryption, or secure boot without a
   separate provisioning, upgrade, rollback, and recovery program.
7. Do not require lab hardware in normal pull-request CI.
8. Do not accept a hardware PASS without typed, machine-validated evidence.
9. After every work package: run its focused tests, inspect the diff, run
   `git diff --check`, and confirm no unrelated tracked files changed.
10. Before the final handoff: run the complete authoritative CI gate and fresh
    hardware checks appropriate to the touched surfaces.

## Work package 1: make sanitizers blocking

Scope: one policy line in `scripts/ci-test.sh`.

- Replace the advisory native-sanitized invocation with `run_step`.
- Do not otherwise change the sanitizer environment or native test runner.

Acceptance:

- All 148 eligible sanitized suites pass.
- The full CI gate passes.
- Production firmware inputs and memory use are unchanged.

Rollback: restore the single invocation to `run_advisory_step`.

## Work package 2: separately compiled native-test pilot

Scope: `AlpEventLatch` only.

- Add an explicit, fail-closed per-suite production-source manifest.
- Use PlatformIO's exact `PIOTEST_RUNNING_NAME` to compile
  `src/modules/alp/alp_event_latch.cpp` as a separate object only for
  `test_alp_event_latch` in linked-pilot mode.
- Retain the existing direct include behind a suite-specific conditional while
  the pilot is dual-run.
- Give linked and linked-sanitized runs separate clean build roots.
- Do not enable `test_build_src = true` and do not migrate a second suite.

Acceptance:

- Legacy, linked, and linked-sanitized variants all pass the same three tests.
- Symbol/object inspection proves the linked definitions come from the
  production-source object rather than the test translation unit.
- No other suite receives the production source.
- Full CI and production firmware size remain unchanged.

Rollback: disable linked-pilot mode; the legacy include remains intact.

Cutover is a later work package after at least three clean full CI runs.

## Work package 3: setup-only dependency injection pilot

Scope: `initializeBootPerformanceLoggers()` only.

- Introduce a narrow `BootLoggingRuntimeServices` reference bundle containing
  the existing storage, settings, perf logger, ALP logger, and GPS objects.
- Construct the bundle at the existing call site in `main.cpp`.
- Replace implicit singleton access inside the helper with those references.
- Preserve call order, logs, boot-token behavior, maintenance-boot branching,
  and all object definitions and lifetimes.

Acceptance:

- No new global/static object and no allocation.
- Extern/global contracts pass.
- Firmware build, memory comparison, cold normal boot, and maintenance boot
  behave as before.
- A fresh core bench passes before this package is considered release-ready.

Rollback: revert the signature and call-site bundle construction.

## Work package 4: bind API writes to maintenance boot

Scope: the existing central write guard and a dependency-free policy.

- Require both `maintenanceBootMode_ == true` and the existing
  `X-V1Simple-Request: maintenance-ui` header.
- Preserve the external 403 response for either rejection and log only the
  internal reason.
- Keep every POST routed through the one central guard.
- Add the complete four-state policy matrix plus a positive allow test.

This protects settings and runtime mutations if a future lifecycle regression
starts the web server during normal runtime. It does not authenticate another
client already admitted to the maintenance network and does not protect
physical flash or SD extraction.

Acceptance:

- Policy unit tests and the WiFi API contract pass.
- All mutating API routes remain centrally guarded.
- Maintenance-mode synthetic checks pass on a device before release.
- No settings, frontend, credential, or migration changes are introduced.

Rollback: revert the central policy call; there is no persisted state.

## Work package 5: typed OBD/proxy release evidence

Scope: release evidence validation only; no runtime or `bench.sh` change.

- Require an `obd-proxy-arbitration` entry in every release evidence manifest.
- Permit exactly one of:
  - typed `hardware-qualification` evidence with result `PASS`; or
  - structured `accepted-risk` evidence with result `ACCEPTED_RISK`, rationale,
    and explicit scope.
- Validate PASS artifacts against a versioned qualification profile containing
  deterministic case IDs, git SHA, firmware version, board/rig identity,
  timestamps, per-case evidence logs, and zero panic/watchdog counts.
- Reject missing, duplicate, failed, path-traversing, wrong-SHA, and arbitrary
  evidence files.
- Run only synthetic validator tests in normal CI. Real evidence remains a
  release/lab responsibility.

Initial case inventory:

- OBD pair/connect and PID activity.
- OBD power-cycle reconnect.
- V1 power-cycle reconnect while OBD is enabled.
- Proxy phone connection.
- Proxy takeover stops OBD activity.
- OBD recovery after proxy-mode exit.
- Sustained OBD window.
- Sustained proxy window.

Acceptance:

- Synthetic positive and negative tests pass.
- Omitting OBD/proxy evidence fails validation.
- An arbitrary existing file cannot satisfy a PASS.
- An accepted-risk waiver remains valid when no representative rig exists.

Rollback: revert the manifest schema/validator package; runtime is untouched.

## Work package 6: immutable CI action references

- Resolve every GitHub Action tag to its current commit through the GitHub API.
- Never type or infer a SHA from memory.
- Use the same resolved SHA for the same action in CI and release workflows.
- Update workflow contract tests to require a 40-hex reference.
- Add weekly Dependabot updates for the `github-actions` ecosystem.

Acceptance:

- Workflow contract and release compliance tests pass.
- `actionlint` passes.
- No mutable action refs remain.
- The GitHub-hosted CI workflow passes before merging.

Rollback: revert only the workflow/reference package.

## Work package 7: advisory coverage and changed-line formatting

Coverage:

- Add a separate `native-coverage` environment and CI job.
- Preserve per-suite isolated build roots.
- Measure only native-test-reachable production source and label the report
  accordingly.
- Keep the job advisory initially; set no floors until the measurement is
  stable and separately compiled tests broaden its meaning.

Formatting:

- Pin the formatter version.
- Check only changed C/C++ lines, with new files checked fully.
- Run advisory first; never rewrite files inside CI and never mass-format the
  existing tree as part of functional work.

Acceptance:

- Existing authoritative CI duration and outcome are unaffected.
- Coverage and formatting failures cannot block until their tools and baselines
  have demonstrated stability.
- Promotion to required checks occurs in separate policy changes.

## Explicitly deferred programs

The following require separate design and recovery work and are not part of
this hardening branch:

- generated per-device AP credentials;
- PIN or authenticated maintenance sessions;
- NVS/flash encryption, secure boot, or key provisioning;
- re-encoding existing NVS or SD credential recovery data;
- camera-based physical panel validation;
- mandatory hardware execution in pull-request CI;
- broad ownership migration or whole-tree formatting;
- removal of all direct production `.cpp` includes.

## Final release gate

The branch is ready for release review only when:

1. Every landed work package has its focused tests and rollback note.
2. `./scripts/ci-test.sh` passes from a clean tree.
3. Firmware/package size and memory headroom are compared with the protected
   baseline and any change is explained.
4. Device suites pass on the target board.
5. Fresh core/display bench evidence is valid for the final commit.
6. OBD/proxy has either typed PASS evidence or an explicit accepted-risk entry.
7. Maintenance-mode API writes succeed on-device and normal-runtime writes are
   unavailable by construction.
