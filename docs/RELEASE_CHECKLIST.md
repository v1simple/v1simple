# Release Checklist

> Status: Active
> Date: 2026-05-27

Use this before merging `dev` to `main`, then run the manual release workflow
from `main` after the merge. The workflow publishes both the GitHub Release
assets and the GitHub Pages ESP Web Tools installer from the same generated
manifest and merged firmware image.

## 1. Branch and version

- Be on `dev`; do not release from `main` directly.
- Working tree clean before the final gate.
- `include/config.h` must contain a plain semver string, e.g. `#define FIRMWARE_VERSION "4.2.0"` — no suffix spaces.
- `CHANGELOG.md` current entry and Version History must match that version.

## 2. Documentation gates

- External route changes are reflected in `docs/API.md`.
- Perf threshold or correctness-query changes are reflected in `docs/PERF_SLOS.md` and the JSON files under `tools/`.
- Hardware/install changes are reflected in `docs/HARDWARE_NOTES.md`.

## 3. Local CI gate

Run the authoritative local gate before committing release-prep changes:

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
      "rationale": "No representative OBD/proxy hardware qualification rig exists for this terminal release; optional feature accepted based on unit/contract/build gates."
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
rationales. A `WARN` bench result requires explicit investigation; use
`--allow-bench-warn` only when that investigation is documented.

## 5. Merge and release procedure

- Push `dev` only when explicitly intended.
- Open PR from `dev` to `main`.
- Merge with a merge commit, not squash, to avoid release/version history conflicts.
- After the merge is on `main`, manually run `.github/workflows/release.yml`
  (`workflow_dispatch`) for the release commit. The workflow performs:
  1. `scripts/ci-test.sh`
  2. Version read from `include/config.h`
  3. Conflicting tag-collision guard; reruns may reuse a tag already pointing at the same commit
  4. Web build + asset checks
  5. Firmware/filesystem build
  6. ESP Web Tools merged image with QIO/80m/16MB image-info validation
  7. GitHub Pages installer staging and static contract validation
  8. Image-info evidence artifact upload
  9. Git tag creation/reuse
  10. GitHub Release
  11. GitHub Pages installer deployment

## 6. Release assets

Releases ship `.bin` artifacts and an ESP Web Tools manifest for USB and web-installer flashing. Verify the release contains:

- `bootloader.bin`
- `partitions.bin`
- `firmware.bin`
- `littlefs.bin`
- `merged-firmware.bin`
- `manifest.json`

The release workflow deploys the `web-installer/` template to GitHub Pages with
the release `manifest.json` and `merged-firmware.bin` beside the page. Verify
the live installer URL serves:

- `index.html`
- `manifest.json`
- `merged-firmware.bin`

The installer page must be served over HTTPS for browser Web Serial access.

`merged-firmware.bin` is built with the same flash policy as `platformio.ini`
(`qio`, `80m`, `16MB`). The release workflow stores an `image_info` report and
fails if those fields drift. The report is uploaded as the
`release-image-info-<tag>` workflow artifact.

Do not ship or instruct users to flash PlatformIO's generated
`firmware.factory.bin`. Direct upload is patched by `scripts/force_app_upload_offset.py`,
and release flashing must use the workflow-produced binaries above, especially
`merged-firmware.bin`, so offsets match `partitions_v1.csv`.

Release readiness still requires local CI plus section 4 bench evidence and any explicit section 4b evidence or accepted-risk waivers before merging.
