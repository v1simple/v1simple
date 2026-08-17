# V1Simple

> Keep V1Simple simple, in code and docs.

**IMPLEMENTED:** Firmware for the Valentine One Gen 2 and the Waveshare
ESP32-S3-Touch-LCD-3.49. It connects to the detector over BLE, parses its
display stream, renders alerts on the board, and can expose a BLE proxy.
Configuration runs in a separate maintenance-mode WiFi interface.

## Hardware and safety

- Waveshare ESP32-S3-Touch-LCD-3.49, **board revision V1**
- Valentine One Gen 2 with BLE enabled

### Board revision — read before ordering

This firmware is built and bench-verified against the **V1** revision only. It
has not been run on V2 hardware.

Waveshare discontinued the V1 board and switched shipments to V2 after
2026-06-08, so a board bought new today is likely V2. V2 swaps the IO for
`LCD_BL`/`EXIO_INT` and for `LCD_TE`/`LCD_RESET`. This firmware hardcodes
`LCD_BL=8` and `LCD_RST=21` in `platformio.ini`.

**Symptom on V2: the screen never lights up.** The board is not damaged and the
flash succeeded — the firmware boots, connects to the detector, and renders into
its canvas, but drives the backlight on the wrong pin. Not a dead unit.

Identify the revision before flashing:

| | V1 | V2 |
|---|---|---|
| PCB silkscreen | Rev1.0 | **Rev1.1** |
| QC sticker on the case | none | **V2** |

The revision numbering is confusing: silkscreen `Rev1.1` is the *newer* V2
board. Waveshare documents this on
<https://docs.waveshare.com/ESP32-S3-Touch-LCD-3.49> only; the older
`waveshare.com/wiki` page for the same product does not mention it.

V2 support is not yet implemented. The web installer publishes a single merged
image for all users, so revision handling cannot be a build-time option and is
an open design question.

### Safety

Use the board's USB connection for the documented bench, flash, and test path.
The `esp32-s3-car-install` build only changes firmware shutdown behavior; it is
not vehicle-power wiring guidance. Any vehicle installation needs a separately
verified, protected power supply and wiring.

The cased board is sold with and without a lithium cell. A lithium battery left
in a parked vehicle can exceed its rated temperature range. Choosing a
no-battery variant for a permanently mounted install avoids that; this project
does not verify thermal behavior in any configuration.

## Build, flash, and use

Install PlatformIO Core 6.1.19 or newer and a Node.js version accepted by
`interface/package.json`. From the repository root:

```sh
./build.sh          # build the web interface and default firmware
./build.sh --all    # build, flash LittleFS and firmware, then monitor serial
```

`--all` overwrites internal LittleFS data. Confirm SD-backed storage before
relying on saved profiles.

On a normal boot, the firmware starts scanning for the detector. Hold and
release **BOOT** after about four seconds to reboot into maintenance mode. Join
the default `V1-Simple` network with `setupv1simple`, open
`http://192.168.35.5`, and change that default password during first setup.

With an SD card mounted, completed V1 alert tables are saved under
`/encounters` for maintenance-mode download. They preserve the detector's own
alert assignments and contain no GPS or vehicle-speed data. On each normal
boot, the firmware keeps the newest 20 generated CSV files in each of `/perf`,
`/alp`, and `/encounters`. Files with other names are never removed by this
retention policy.

The non-destructive setup and acceptance criteria for comparing a 32 KB FAT32
allocation unit are documented in [`docs/SD_STORAGE.md`](docs/SD_STORAGE.md).

## Verify a change

Install and verify the fail-closed privacy boundary once per clone before making
any commit:

```sh
./scripts/setup-hooks.sh
./scripts/check_local_privacy_setup.py
```

The setup fixes this checkout's author and committer identity to the public
project identity, installs early and final privacy gates that remain active for
`git commit --no-verify`, and pins ordinary Git pushes and IDE Sync to the
verified public `origin`. Every normal push passes through the range-aware
history scanner, which checks all newly reachable blobs once by object hash as
well as commit and tag metadata. Never use `--no-verify` or override the hooks
or remote configuration. The local checker also requires an owner-only private
term list at
`~/.config/v1simple/privacy_terms.txt`; keep personal names, addresses, network
names, device identifiers, and other site-specific terms there, one per line.
Never add that file or its values to this repository.

```sh
./scripts/ci-test.sh                   # complete local code, test, and build gate
./scripts/run_device_tests.sh --quick  # connected-board boot and heap checks
./bench.sh --all --camera              # core + display + replay + camera, one verdict
```

The unified bench command builds and manages `v1replay` as its V1 detector: an
idle LightBlue-compatible V1 for the core and display windows, then the fixed
multi-alert scenario for replay. Before the scored replay window, it launches a
quiet handshake-only peripheral, requires one complete startup epoch in a
separate ledger, fences the still-open serial session, stops that exact process,
and waits for the board's V1-disconnect cleanup marker. Only then does it start
camera recording, QSTART, and the replacement replay peripheral on the same
board boot. The two ledgers are graded independently, so events cannot cross the
managed disappearance boundary. The replay uses its provisional scenario blink
profile by default: the priority arrow blinks during the authored multi-alert
interval and stays steady during single-alert periods. Use
`--blink-profile steady` or `--blink-profile stress` for explicit controls. A
physical V1 is not required. After an upload,
it allows 90 unscored seconds for one-time post-flash SD activity to settle;
the scored session still retains the same SD-start and runtime limits. It then
captures calibrated camera evidence for every live window and stores the logs,
metrics, media, and `bench_result.json` under one `.artifacts/bench/.../runs/`
directory. Camera evidence has one explicit role per suite:

- Core video is diagnostic capture only. It cannot change the verdict.
- Display video records the deterministic preview exercise. It is useful for
  inspection and grader development, but it is not replay evidence and cannot
  change the verdict.
- Replay video is the gated end-to-end display validator. Replay artifacts also
  include that boot's encounter CSV; the grader checks alert timing, primary
  frequency, and direction against that same-window log. Two separate bounded,
  anonymous handshake ledgers record only the automatic startup transactions;
  the grader independently checks framing, checksums, same-session order, short
  characteristics, CoreBluetooth-accepted replies, and the fresh post-cleanup
  epoch. The replacement metrics window must also contain exactly one canonical
  all-volume parser commit. Within that bounded session, the decoded ledger and
  parser-counter edge provide integration evidence that a canonical reply from
  the controlled emulator reached board parser state, without adding packet
  data or device identity to the CSV. They are not general packet traces and
  contain no central identifier or timestamp. The grader also reconstructs the
  managed exit, board cleanup, serial fences, same-boot replacement, and bounded
  three-packet preflight from the fixed same-window logs instead of trusting
  collector summary flags.

Every requested camera window applies the fixed 1280x720, 200 fps UVC profile
and exposure 50, records a session-start still, and admits the run only when
the SCAN landmark produces a fully contained, bounded dynamic scale/position
crop. Core and display do this before opening serial or starting their emulator.
Replay does it after the unscored reconnect cleanup fence and immediately before
QSTART, so process A cannot consume the duration-bounded recording. A refusal writes
`camera_preflight.json` with measured camera diagnostics and ends as
`EVIDENCE_FAILED`; it does not claim firmware failure. The successful preflight
hash and exact normalized crop are owned by the immutable capture manifest.
The native recorder discards stale capture callbacks and writes delivered image
buffers on a monotonic host-clock timeline instead of trusting camera-supplied
sample timestamps. During gated replay, a live writer failure is surfaced
immediately, aborts the incomplete collection, and remains an evidence failure;
it is never converted into a product failure or hidden by a later generic
video-probe error. Core/display capture failures remain diagnostic/exercise-only.
The completed video is still probed for its real dimensions and average frame
rate; an apparent 200 fps request that actually records below 199 fps is rejected.

Run the same camera-only lifecycle as a short standalone smoke, without serial,
upload, emulator, or a long collection window:

```sh
python3 scripts/bench/camera_preflight.py --out-dir <new-output-directory>
```

The artifact embeds the camera contract, including the bounded calibration
controls and fixed oracle thresholds. Bounded crop scale/position and bounded
timeline alignment may self-calibrate. Frequency recognition decodes the five
seven-segment digits directly from the normalized close crop; it has no stored
reference pictures and abstains when segment occupancy is ambiguous. The fixed
exposure, artifact ownership, same-window encounter log, and match thresholds
remain fixed. Replay alignment is anchored to the first sample actually emitted
after BLE becomes ready, not to emulator process launch.

A valid replay image/log disagreement is `FAIL`. Missing, unowned, unalignable,
or otherwise ungradable camera evidence is `EVIDENCE_FAILED`: it blocks the
unified gate without claiming that firmware behavior failed, and collection is
still reported separately. The video and exposure stills remain archived for
diagnosis, but human viewing is not the acceptance gate.

Each window also records four content identities in `identity.json`. The
product fingerprint covers firmware, production configuration and build hooks,
checked-in UI/audio/branding sources, dependency pins, and UI build/deploy
inputs, directly deployed audio/branding assets, and the complete `v1replay`
implementation. Generated UI build output is represented by those checked-in
inputs rather than hashed directly. The hardware-scoring fingerprint covers
metric contracts, import and derivation, scoring, reporting, collection and
verdict integration, and qualification policy. Any change to it requires fresh
hardware evidence. The grader fingerprint covers camera capture, evidence
ownership, dynamic registration, and the reference-free seven-segment grader.
A separate scenario fingerprint covers the suite, duration, profile, segment,
and replay blink profile. Git SHA/ref and clean state remain traceability only.
Promoted performance baselines live under
`<board>/<product fingerprint>/<hardware-scoring fingerprint>/<suite>/<scenario fingerprint>/`;
an older or scoring-incompatible baseline is never selected automatically.

Ask the read-only qualification planner for the minimum evidence work before
starting a bench run. It prints commands but never executes them:

```sh
python3 scripts/bench/bench_policy.py plan \
  --qualification <accepted-qualification.json>
```

A product, per-suite scenario, or hardware-scoring change requires the full
batch. A camera-grader-only change requires a complete archive regrade plus one
live camera smoke. Matching product, scenarios, hardware scoring, and grader
reuse the accepted evidence; Git SHA/ref changes remain traceability only.
Missing, legacy, or malformed qualification records default to the full batch;
because accepted records are immutable, migration publishes a distinct new
qualification record. `record-full` accepts only a clean
core/display/replay PASS with strict replay camera ownership, and
`record-grader` advances only the grader after a complete regrade report, a
current smoke PASS, and a confident current grade for the previously accepted
replay capture.

### Evaluate a firmware optimization

`scripts/improve.py` is the Phase-B, host-side controller for comparing one
committed firmware candidate with its pinned base. It never changes the
invoking worktree or the submitted candidate branch. Instead it creates owned
evaluation worktrees and an `improve/...` evaluation branch, builds both
revisions before reserving the rig, flashes firmware without replacing
LittleFS, and runs the fixed all-suite/camera gate with no promoted baseline.
The controller holds the same managed-V1 radio lease used by ordinary bench
windows from the first flash through the last run, so another bench cannot
interleave with a candidate campaign.

A live experiment requires at least five fresh runs per arm. Baseline and
candidate runs are time-balanced in `B,C,C,B` blocks. Every individual batch
must be a clean canonical PASS with a strict replay camera grade, and flash,
RAM, IRAM, and DIRAM must remain inside the existing production limits. The
target policy and direction come from the checked-in hardware metric catalog;
informational metrics and changes to metric instrumentation are not eligible.
The first controller version is replay-only and supports only
`disp_pipe_p95_us` and `disp_pipe_max_peak_us`. Its on-device timing boundary,
headers, pre-timer orchestration, importers, scorer, and camera gate are
protected. A candidate may change only the implementation `.cpp` files named
by `DISPLAY_OPTIMIZATION_PATHS` in `scripts/improve.py`, plus accompanying
tests; at least one eligible implementation file must change. The pinned base
must be the clean invoking `HEAD`, and the candidate must be its single direct,
non-merge child.
Acceptance requires strict empirical-envelope separation: every candidate
sample must beat every baseline sample. An equal or overlapping envelope is a
rejection, even when the candidate median is better. Each candidate suite is
also rescored against all N fresh, same-suite baseline manifests, so improving
the target cannot hide a regression in another hard or advisory metric.

First exercise the controller without build, serial, camera, or hardware
access. This deterministic proof uses real Git only inside a disposable
temporary repository. It verifies a committed behavior-no-op candidate takes
the same pre-flash `REJECTED_NO_CHANGE` path as a byte-identical live build,
including the owned evaluation worktree and real revert; separate simulated
scenarios exercise acceptance and crash recovery:

```sh
python3 scripts/improve.py dry-run
```

Every live `start` repeats that dry run before creating its immutable plan and
stores the exact report and controller-component hashes with the session.
The default five-run-per-arm campaign requires at least 38.5 GiB free at
startup: ten estimated 2.25 GiB full-camera runs, 6 GiB for builds/worktrees,
and a 10 GiB safety reserve. Space is checked again before every capture.

Then evaluate a single committed firmware candidate branch:

```sh
python3 scripts/improve.py start \
  --candidate-branch <local-branch> \
  --target-suite replay \
  --target-metric disp_pipe_p95_us \
  --runs 5 \
  --port <serial-port> \
  --artifact-root <durable-artifact-root>
```

The controller records an immutable plan and decision, a hash-chained event
journal, resource and firmware hashes, and every bench result and strict
qualification it cites. A rejected or interrupted candidate is followed by a
verified base-firmware restore and a revert commit on the controller-owned
evaluation branch; the submitted branch remains intact. After an ungraceful
host stop, `resume --session <path>` performs recovery only—it restores the
base and closes the experiment rather than selectively resuming measurements.
Failed restore or evaluation-branch cleanup remains durably blocked and must
be retried with `resume`; it never unlocks another experiment. A reserved
recovery block protects that closeout from a full artifact volume, and free
space is rechecked before every full-camera capture using the remaining run
count and observed artifact sizes.
An `ACCEPTED` candidate remains isolated and pending user integration;
promotion or merging is a separate human decision.

#### First live evaluator closeout

The first live five-run-per-arm evaluation completed ten identity-owned,
canonical `PASS` batches. All 30 suite windows passed, and all ten gated replay
camera grades were confident `PASS`. The lower-better target was
`replay.disp_pipe_p95_us`. Baseline min/median/max were
38,615.85/38,640.00/39,085.00 us; candidate min/median/max were
38,324.75/38,535.00/38,944.45 us. The candidate median was 105 us lower, but
its envelope overlapped the baseline by 328.60 us, so the stored analysis did
not satisfy strict empirical-envelope separation and the candidate was not
integrated.

The immutable controller decision remains `REJECTED_GATE_FAILURE`; it is not
retroactively relabelled. A post-analysis `git revert` attempted to update an
`AUTO_MERGE` tree ref that the privacy hook correctly refused. The fallback
cleanup still restored the baseline firmware, reverted the controller-owned
evaluation branch to the pinned base tree, and preserved the submitted
candidate branch. The session recorded `evidence_integrity=PASS`, and its final
state recorded no pending firmware restore or evaluation cleanup. The
scientific closeout is therefore `NO_GO` for this candidate, kept separate from
the official immutable result.

The external session remains the evidence authority. Its portable anchors are:

- plan SHA-256: `a58fa9cebdf844e9d5d4f32708a29293424907507ed1a6f5add4a6e6a638adbd`
- decision SHA-256: `74291e43613ccfcec9bf886f8085415e29cef2861b331c6b6a053307f931e8b4`
- final journal event SHA-256: `13f867aa654472bfd4096758bae745eb62679059881745e5890b20d6a33dde3a`

Commit `809be3633e3cc222928b7a5b85a217f94dce85c5` made clean evaluation
finalization compatible with the privacy hook and ensures a recovered cleanup
cannot mask an already-recorded negative analysis. The original session was
not rewritten. Two later host-only planning screens were also stopped before
implementation: steady-state safety-flush elimination projected 0 us of p95
improvement on all five baseline traces, and an RSSI-only raster cache had a
generous 630 us ceiling against the 1 ms preflight bar. Those screens are
diagnostic planning results, not qualification evidence. No follow-on
candidate justified another live campaign.

Bench metrics have independent absolute and baseline-regression checks. The
`absolute_min` and `absolute_max` fields in
[`tools/hardware_metric_catalog.json`](tools/hardware_metric_catalog.json)
control the absolute check. When both are `null`, the scorer reports
`absolute_state=n/a`. A hard policy may also set `advisory_min` or
`advisory_max` inside its absolute bounds; crossing that earlier line warns the
run while crossing the absolute bound still fails it. Fully unbounded entries
are allowed only for informational observations; they may remain required when
the evidence itself is mandatory but no causal value threshold is known. Hard
and advisory policies require an absolute contract. Other required metrics may
also have a compatible `regress_abs` or `regress_pct` comparison: sustained
Wi-Fi and display work use a 50 ms ceiling, while an individual Wi-Fi peak uses
the main loop's 250 ms ceiling. The `required` field independently enforces
presence: a missing required metric fails the run regardless of its value
thresholds.

The three SD write-peak metrics are required health telemetry, not
product verdicts. Recorded bench and camera evidence has not identified a raw
SD-duration value that causes receive or display failure, and `sdMax_us`
times the append/flush path, including SD-lock wait and any preemption during
that interval. Qualification therefore gates the measured consequences
instead: missing or transfer-corrupt CSV evidence, explicit perf/event/packet
drop counters, parser failures, receive/display continuity, reboots, and the
gated replay camera contract. SD peaks remain in the scoring artifacts, with
compatible-baseline trends in the comparison artifacts, for diagnosis.

When supported by the source, `notify_to_display_max_ms` is likewise diagnostic
telemetry, not a standalone product verdict. It measures BLE notification
arrival to display-pipeline dispatch; it does not measure when changed pixels
become visible. Recorded evidence has not established a raw maximum that
independently causes packet loss, parser failure, a stale or incorrect display,
or a missed replay encounter. The value and compatible-baseline trend remain
available for diagnosis, while direct receive, drop, replay-semantic, and
same-window visual witnesses own the verdict. A future hard response-time
contract requires a defined product deadline and frame-synchronized transition
evidence that measures visible pixels against that deadline.

Automated tests establish code behavior. Device tests and bench runs establish
only what happened on the connected setup. Camera evidence establishes visible
screen behavior for that recorded run. None proves every detector, power, RF,
or vehicle environment.

Keep changes focused, read [AGENTS.md](AGENTS.md), run the complete gate, inspect
the final diff, and say whether hardware or camera evidence was collected.

## Choose the next release

Every successful reviewed publication to public `main` publishes a release.
Ordinary pushes and IDE Sync use the same verified privacy gate. Patch is the
default.
After committing the changes to release, select a minor or major bump before
the reviewed publication:

```sh
./scripts/release-bump          # show the selection and next version
./scripts/release-bump minor    # select 1.2.0 after 1.1.x and commit the choice
./scripts/release-bump major    # select 2.0.0 after 1.x and commit the choice
./scripts/release-bump patch    # explicitly select the normal patch release
```

The selector refuses to run with other uncommitted changes and creates the
release-selection commit for you. Do not edit `FIRMWARE_VERSION`; the release
workflow owns it. After a successful minor or major release, the workflow resets
the selection to patch in its release commit, so sync `main` before continuing
development.

## Project notices

MIT licensed; see [LICENSE](LICENSE). See [SECURITY.md](SECURITY.md) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). This independent project is
not affiliated with or endorsed by Valentine Research, Inc.
