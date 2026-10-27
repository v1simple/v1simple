# Building and maintaining V1Simple

For installation and everyday use, start with the [README](../README.md).
Run the commands below from the repository root.

## Build from source

Install pioarduino Core 6.2.0 and LittleFS Python support in the same Python
environment, plus a Node.js version accepted by `interface/package.json`:

```sh
python3 -m pip install 'pioarduino==6.2.0' 'littlefs-python==0.18.0'
```

From the repository root:

```sh
./build.sh          # build the web interface and default firmware
./build.sh --upload # update firmware and web interface, preserving stored data
./build.sh --all    # build, flash LittleFS and firmware, then monitor serial
```

Before building the interface, `build.sh` checks LittleFS support in the Python
interpreter reported by the selected PlatformIO executable, then prepares that
environment's platform and framework packages with `pio pkg install`. The first
build needs network access for project and npm dependencies. If using an existing
PlatformIO installation, set `PIO_CMD` to its executable; a missing LittleFS
dependency reports the installation command for that exact Python interpreter.

`--skip-web` and direct `pio run` reuse only an unchanged, verified frontend
build. If it is missing or stale, run `npm run build` in `interface/` first.

`--all` overwrites internal LittleFS data. Confirm SD-backed storage before
relying on saved profiles.

## Verify a change

Set up the repository privacy boundary once per clone, and verify it before
working or committing:

```sh
./scripts/setup-hooks.sh
./scripts/check_local_privacy_setup.py  # quick local identity, hook, index, and destination check
```

Never bypass the hooks or remote checks. The local checker requires an
owner-only private-term list at `~/.config/v1simple/privacy_terms.txt`; keep
site-specific names, addresses, networks, and device identifiers there, never
in this repository.

```sh
./scripts/ci-test.sh                   # complete pre-push/release code, test, and build gate
./scripts/run_device_tests.sh --quick  # connected-board boot and heap checks
./bench.sh --replay --camera          # raw replay/camera evidence, then sampled visual fields
```

Automated tests establish code behavior. Device tests and bench runs establish
only what happened on the connected setup. Camera evidence establishes visible
screen behavior for that recorded run. None proves every detector, power, RF,
or vehicle environment.

The bench collector preserves replay input, delivery, timing, serial, firmware
identity, settings snapshot, and camera artifacts. After `COMPLETE` confirms raw
capture, a separate report checks sampled stable display fields against the
recording and prints `VISUAL_FIELDS_PASS`, `FAIL`, or `INCONCLUSIVE`. The report
lives beside `replay/` as `visual_fields_result.json`; a mismatch or inconclusive
reading returns a nonzero status without rewriting the completed raw capture.
This is not a verdict on every video frame or profile-based hold timing. An
unavailable maintenance snapshot makes a collection incomplete, not a product
failure.

Preview old camera videos with `python3 scripts/cleanup_camera_videos.py`; add
`--apply` to remove them. The command keeps 14 days of video under
`.artifacts/bench`, preserves reports, logs, timing files and still images, and
records removals beside each affected run. It skips unfinished runs, symlinks,
and open or recently modified videos; deletion requires a working `lsof`.
If cleanup is interrupted between deletion and recording it, the record retains
that file as pending rather than reporting an unverified removal.
The command runs once; schedule it locally for recurring cleanup.

Keep changes focused, read [AGENTS.md](../AGENTS.md), run checks proportionate to
the change, inspect the final diff, and state whether hardware or camera
evidence was collected. Run the complete gate before a push or release.

For maintenance, the [behavior traces](behavior/README.md) connect the
repaired profile, protocol, ALP and camera paths to their owners and focused
tests, with explicit limits on what each fix proves and a bounded agent prompt.

## Releases

Every successful reviewed publication to public `main` publishes a release.
Ordinary pushes and IDE Sync use the same verified privacy gate. Patch is the
default and is derived from the newest strict semantic-version tag. To select
the next minor or major, change `FIRMWARE_VERSION` in the reviewed product
change to exactly that next version. Arbitrary version jumps are rejected.

Release builds inject the selected version, verify it in the firmware binary,
and tag the exact commit that passed CI. Automation does not create a release
commit or modify source files. [CHANGELOG.md](../CHANGELOG.md) is the curated
summary; complete generated notes and history live in GitHub Releases.
