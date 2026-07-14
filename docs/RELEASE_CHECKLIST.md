# Release Checklist

> Status: Active
> Date: 2026-05-27

Use this before merging a release-ready branch to `main`. Every green merge
automatically publishes the next patch release.
The workflow prepares the version commit and publishes both the GitHub Release
assets and the GitHub Pages ESP Web Tools installer from the same generated
manifest and merged firmware image. There is no second release button or
version choice in the normal path.

Repository setup keeps the automation identity narrow: the write-enabled
deploy key named `v1simple release publisher` is stored only in the
`RELEASE_DEPLOY_KEY` Actions secret, and the `main-pr-ci-gate` ruleset grants
DeployKey bypass only for the generated fast-forward release commit. Human
owners are not on the bypass list. The generated commit uses `[skip ci]` so its
push cannot recursively start another Release run.

## 1. Branch and version

- Prepare changes on a short-lived branch and merge them to `main` through a PR.
- Working tree clean before the final gate.
- Do not manually reuse or move a published version tag. The Release workflow
  derives the next stable semver from immutable tags.
- Every merge uses a `patch` bump automatically. The workflow updates
  `include/config.h`, opens the new changelog section, and rotates changelog
  links automatically. A future minor/major policy should be added through a
  normal reviewed PR rather than an extra publication path.

## 2. Documentation gates

- External route changes are reflected in `docs/API.md`.
- Perf threshold or correctness-query changes are reflected in `docs/PERF_SLOS.md` and the JSON files under `tools/`.
- Hardware/install changes are reflected in `docs/HARDWARE_NOTES.md`.

## 3. Local CI gate

Run the authoritative local gate before merging release-ready changes:

```bash
./scripts/ci-test.sh
```

Do not commit a change unless this passes.

The CI gate writes memory headroom JSON under `.artifacts/test_reports/memory-headroom/`.
If IRAM is reported at zero headroom, flag it in the release PR before any framework, board package, BLE, display, or ISR/hot-path dependency changes are merged.

## 4. Hardware evidence

### 4a. Core/display bench

For release-board confidence, run at least one bench evidence pass after the final commit. The bench suite scores core/display metrics from SD perf CSV exports, not live Wi-Fi polling:

```bash
./bench.sh
```

Expected release-ready shape:

- final `./bench.sh` result: `PASS`
- no `COLLECTION_FAILED`
- no `FAIL`
- any `WARN` is investigated before release.

### 4b. OBD/proxy/arbitration evidence when touched

For releases that touch OBD, proxy mode, connection arbitration, or shared BLE scheduling, run the full hardware-mode OBD/proxy evidence pass and record the artifact path in the release PR or release notes when a representative hardware rig exists.

If no representative OBD/proxy hardware rig exists for this terminal release, do **not** treat missing hardware evidence as an implicit blocker and do **not** remove the already-integrated optional feature late in release prep solely to satisfy this checklist. Instead, record an explicit accepted-risk waiver in the release evidence manifest and release notes:

- result: `ACCEPTED_RISK`
- rationale: no stable OBD/proxy hardware qualification path exists for this release
- scope: OBD, proxy mode, connection arbitration, or shared BLE scheduling areas covered only by unit/contract/build gates

Any actual OBD/proxy hardware pass that is run should still be recorded. Treat any executed hardware step result other than `PASS` or `NO_BASELINE` as a release blocker until it is triaged and documented.

A claimed `PASS` must use the versioned case inventory in
`tools/obd_proxy_qualification_profile_v1.json`. The qualification artifact
must identify the release commit, firmware, DUT, and rig; cover every required
case exactly once; cite a nonempty relative evidence log for each case; and
report zero watchdog resets and panics. Validate it before adding it to the
release manifest:

```bash
python3 scripts/check_obd_proxy_qualification.py \
  --artifact .artifacts/obd-proxy/<run-id>/qualification_result.json
```

### 4c. Evidence manifest

Before merging, write a local evidence manifest that points at the bench result
and any extra hardware artifacts required by section 4b:

```json
{
  "schema_version": 1,
  "evidence": [
    {
      "id": "core-display-bench",
      "kind": "bench",
      "result": "PASS",
      "artifact_path": ".artifacts/bench/release/runs/<run-id>/bench_result.json"
    },
    {
      "id": "obd-proxy-arbitration",
      "kind": "accepted-risk",
      "result": "ACCEPTED_RISK",
      "rationale": "No representative OBD/proxy hardware qualification rig exists for this terminal release; optional feature accepted based on unit/contract/build gates.",
      "scope": ["OBD", "BLE proxy", "connection arbitration"]
    }
  ]
}
```

Validate it with:

```bash
python3 scripts/check_release_evidence_manifest.py \
  --manifest .artifacts/release_evidence/manifest.json
```

The manifest stays local under `.artifacts/`, but the release PR/release notes
should cite the manifest path plus the underlying artifact paths or accepted-risk
rationales and scope. `obd-proxy-arbitration` may not be omitted: it must be a
validated `hardware-qualification` PASS or a structured `accepted-risk` entry.
A `WARN` bench result requires explicit investigation; use `--allow-bench-warn`
only when that investigation is documented.

## 5. Merge and release procedure

- Push the release-ready branch only when explicitly intended.
- Open a PR from the release-ready branch to `main`.
- Merge with a merge commit, not squash, to avoid release/version history conflicts.
- After the required PR check passes, merge to `main`. Every merge starts
  `.github/workflows/release.yml` automatically with a patch bump. The
  workflow:
  1. refreshes current `main` and immutable version tags
  2. selects the exact merged commit, safely peeling only annotated two-file
     release commits when resuming the same run
  3. applies the next patch bump; only the same recorded workflow run may reuse
     an existing tag
  4. prepares `FIRMWARE_VERSION` plus `CHANGELOG.md` and creates a local release commit
  5. requires that commit to be a direct child changing exactly those two files,
     with `FIRMWARE_VERSION` the only changed configuration value
  6. builds and validates the frontend, firmware, and filesystem once through
     `scripts/build_production_artifacts.sh`
  7. validates the ESP Web Tools merged image with the DIO/80m/16MB policy
  8. stages the GitHub Pages installer, notices, and runtime licenses
  9. atomically pushes the fast-forward release commit and its single tag
  10. publishes generated GitHub release notes and binary assets
  11. deploys the GitHub Pages installer

If a run fails, use **Re-run jobs** on that original Actions run so its run ID
is preserved. The same version is prepared again. If publication already
pushed the release commit and tag, the rerun finds that run's annotated tag and
resumes the exact tested commit. It may repair that release's missing assets,
but it deploys Pages only when its tag is still the newest release, so a retry
cannot roll the live installer backward. If `main` advances during a release
build or publication, the older run refuses the race; the newer merge has its
own queued release run.

## 6. Release assets

Releases ship `.bin` artifacts and an ESP Web Tools manifest for USB and web-installer flashing. Verify the release contains:

- `bootloader.bin`
- `partitions.bin`
- `firmware.bin`
- `littlefs.bin`
- `merged-firmware.bin`
- `manifest.json`
- `THIRD_PARTY_NOTICES.md`
- `ArduinoJson-LICENSE.txt`
- `NimBLE-Arduino-LICENSE.txt`
- `NimBLE-Arduino-NOTICE.txt`
- `Arduino-GFX-LICENSE.txt`
- `OpenFontRender-LICENSE.txt`
- `FreeType-FTL.txt`
- `Svelte-LICENSE.md`
- `SvelteKit-LICENSE.txt`
- `daisyUI-LICENSE.txt`
- `Tailwind-CSS-LICENSE.txt`

The release workflow deploys the `web-installer/` template to GitHub Pages with
the release `manifest.json` and `merged-firmware.bin` beside the page. Verify
the live installer URL serves:

- `index.html`
- `manifest.json`
- `merged-firmware.bin`
- `THIRD_PARTY_NOTICES.md`
- `licenses/ArduinoJson-LICENSE.txt`
- `licenses/NimBLE-Arduino-LICENSE.txt`
- `licenses/NimBLE-Arduino-NOTICE.txt`
- `licenses/Arduino-GFX-LICENSE.txt`
- `licenses/OpenFontRender-LICENSE.txt`
- `licenses/FreeType-FTL.txt`
- `licenses/Svelte-LICENSE.md`
- `licenses/SvelteKit-LICENSE.txt`
- `licenses/daisyUI-LICENSE.txt`
- `licenses/Tailwind-CSS-LICENSE.txt`

The installer page must provide visible links to the notices and license texts,
and must be served over HTTPS for browser Web Serial access.

These assets cover the currently identified direct firmware libraries and
direct web-runtime packages only. They do not close the separate
component-level inventory for the Arduino/ESP-IDF framework, linked compiler
runtimes, or transitively emitted web modules; do not describe the release as
having a complete dependency-license inventory until that residual review is
finished.

`merged-firmware.bin` is built with the same flash policy as `platformio.ini`
(`dio`, `80m`, `16MB`). The release workflow stores an `image_info` report and
fails if those fields drift. The report is uploaded as the
`release-image-info-<tag>` workflow artifact.

Do not ship or instruct users to flash PlatformIO's generated
`firmware.factory.bin`. Direct upload is patched by `scripts/force_app_upload_offset.py`,
and release flashing must use the workflow-produced binaries above, especially
`merged-firmware.bin`, so offsets match `partitions_v1.csv`.

Release readiness still requires local CI plus section 4 bench evidence and any explicit section 4b evidence or accepted-risk waivers before merging.
