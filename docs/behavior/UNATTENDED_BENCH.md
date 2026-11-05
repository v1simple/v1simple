# Unattended behavior bench

`behavior-bench.sh` exercises the installed production firmware without flashing.
It operates the actual maintenance website in Chrome, reads the saved settings
independently, records the physical display, reboots, and repeats the observation.
The alert suite sends controlled BLE alerts, a final clear, and then stays
connected without further notifications to observe disappearance and a fresh alert.

The fixed bench needs USB, its admitted camera view, the existing built `v1replay`
host tool, an installed Google Chrome, and access to the DUT maintenance Wi-Fi.
The wrapper installs pinned Python dependencies before opening the device. Camera
and Bluetooth permissions must already be available. No button presses are needed
once that bench is set up. Run only with the bench device and intended emulator in
range; the HTTP address must identify that same USB DUT.

Use the installed firmware's USB-reported git and image identities. Save private
evidence to an absolute path outside any Git checkout:

```sh
./behavior-bench.sh run --git <installed-git> --image <installed-image> \
  --out-dir <private-new-run-directory>
```

Select USB explicitly with `--port` if several devices are attached. The default
URL and Wi-Fi name are the standard maintenance defaults; `--base-url` and `--ssid`
override them. `--suite colors` or `--suite clearance` runs one half. `--headed`
makes the automated Chrome window visible. The complete run requires 12 GB free
before recording; colors require 7 GB and clearance requires 5 GB. Capture also stops before disk
space falls below 1 GB. Original movies are never deleted automatically.

`--suite profiles` exercises the shipping website's New, description Edit,
duplicate-name rejection from a naturally stale second page, reboot retention,
and Delete. It saves every complete original profile before creating one uniquely
named temporary profile. Cleanup deletes only an exact document owned by this run,
and refuses deletion if that profile acquired a slot reference or an external edit.
This suite uses 1 GB of free-space allowance and does not require a camera.

`--suite acceptance` combines profiles, colors, clearance, and five externally
recorded speaker cases: Ka speech, detector-zero silence, X speech, speaker-zero
silence, and audible recovery. Every speaker case starts on a fresh normal boot;
the camera independently checks the associated alert and its disappearance. The
existing default `all` continues to mean colors plus clearance.

`--suite audio` runs those same five speaker cases and their ten acoustic/optical
checks on their own. It reuses the same continuous camera, original-state journal,
fresh-boot sequence, and verified restoration as acceptance, without repeating
profiles, color previews, or the separate clearance suite. It requires 12 GB free
per cycle and the same candidate, microphone, and real-detector-off admission.

`--suite warning` is a camera-only zero-detector-volume regression. It sends the
fixed nine-second K encounter after seven seconds of connected idle, clears it,
and holds the connection for 30 seconds
without further notifications. Four checks require stable idle controls, the
15-second warning deadline, repeated complete red `VOL 0` glyphs alternating with
blank phases at the 250 ms cadence, and the original placeholder returning after
the ten-second warning. It temporarily silences the speaker, restores all owned
settings, and never starts a microphone recorder. It requires 5 GB, a qualified
candidate, and `--real-v1-off`; `all` and `acceptance` keep their existing scope.

For audio, acceptance, or warning, retain the qualified firmware, ELF, and their `identity.json`
manifest together. The manifest must name the exact full commit, successful CI
result and exit code, image identity, and SHA-256 of its files. The repository's
existing exact-commit CI marker must match. The runner checks the application
descriptor against the ELF and independently pins the USB-reported installed
identity. It records the current host tools, dependencies, and executable hashes
separately; modified host tools are not represented as CI-qualified firmware.

```sh
./behavior-bench.sh run --suite acceptance --cycles 2 \
  --git <qualified-commit> --image <installed-image> \
  --firmware <private-retained-candidate>/firmware.bin \
  --microphone <named-microphone-input> --real-v1-off \
  --out-dir <private-new-campaign-directory>
```

The real detector must remain off throughout emulator testing, and the named
microphone must hear the DUT speaker. Acceptance reserves 24 GB per requested
cycle. Each cycle restores and rechecks the original configuration before the
next begins. A failure or incomplete cycle stops the campaign; no partly completed
mutation phase is retried. One shared DUT lease covers serial reopen gaps and all
cycles, preventing another behavior run from taking the device between phases.

Audio capture uses the native macOS recorder with microphone permission already
granted. It retains each source buffer's timestamp and sample count, the local
WAV, and the capture-clock mapping. Missing samples, writer backpressure, early
exit, or incomplete observation coverage stop qualification. Acoustic checks
compare the shipped phrase and individual words, reject competing words or extra
speech, and account for measured clock uncertainty and analysis resolution.
Silence needs visible live alerts and audible controls before and after it; an
unavailable or digitally flat microphone cannot pass a silence case.

The colors suite checks two different custom frequency colors, band-color mode,
all six primary and secondary meter colors, independent Save readback, and
retention across reboots. The clearance suite tests zero- and three-second
persistence, secondary-card expiry, a primary clear followed by connected silence,
and a fresh alert on the same BLE connection. Pixel observations require visible
control regions and distinct frames; a black screen or missing evidence cannot
pass as successful disappearance. The declared timing allowance is an optical
observation budget, not a measured device-receipt latency guarantee.

Clearance checks cover the frequency, arrows, main meter and secondary cards.
The counter and status strip provide visibility controls. The silence test first
retires both protocol owners: the detailed alert table and the separate display
packet. A table-clear packet alone does not mean the detector's display is idle.

Before changing any setting, the runner saves the configuration and USB profiles
and durably records each intended change. Cleanup restores only those owned
fields, then reboots and compares settings and profiles against the original
snapshot. A conflicting external edit prevents an overwrite. There are no
factory resets, complete-backup imports, credential changes, or detector Apply
operations. Evidence contains private settings and is created with private file
permissions.

Slot writes wait for a successful read of the assigned profile while a background
SD backup is busy. Only reads are polled; the mutation still has one attempt.
Restoration runs after the recorder stops and before video verification, so media
processing cannot delay returning the DUT to its original configuration.

`result.json` reports `PASS`, `FAIL`, or `INCOMPLETE`. A pass requires every selected
check plus verified restoration. A contradicted behavior remains a failure even
if cleanup also fails. Collection problems and ambiguous writes are incomplete;
they do not establish a firmware defect. The runner never repeats an uncertain
browser Save. USB logs, browser evidence, camera originals, timing, pixel readings,
and restoration differences remain available for inspection.

After a killed host process or unplugged device, reconnect the same device and run:

```sh
./behavior-bench.sh restore --out-dir <existing-private-run-directory>
```

This uses the durable recovery record and rechecks the saved firmware and USB
identity and saved connection settings. It does not rerun the test.
For a campaign, the same command targets only its recorded interrupted cycle.
`restoration-result.json` reports recovery only; it never replaces the qualification
verdict in `result.json`. To reread a complete capture without hardware:

```sh
./behavior-bench.sh analyze --out-dir <existing-private-run-directory>
```

The report names its required cases and its exercised scope. Missing physical
touch/BOOT actuation, controlled supply faults, real-detector Apply, and physical
ALP/GPS/OBD fixtures remain explicitly unexercised; they cannot become a full
hardware pass. Real-detector Apply is separate from this real-detector-off
campaign. The acoustic checks use recorded sound against the shipped source clips;
camera-only suites do not claim audio coverage. Offline `analyze` rereads individual
display-only suites and does not replace the original qualification verdict. It
refuses acceptance/profile/audio runs and campaigns rather than giving a partial pass.
The separate warning suite supports offline `analyze` with its original declared
timing allowance and verified camera/transport evidence.
Keep release decisions tied to the exact firmware identity and the existing full
CI/release checks; a host-tool test does not qualify a different installed image.

Host regression checks (no DUT):

```sh
"$(./scripts/bench_python.sh --visual)" -m unittest discover -s scripts -p 'test_behavior_*.py'
BEHAVIOR_BROWSER_LIVE_TESTS=1 "$(./scripts/bench_python.sh --browser)" \
  -m unittest discover -s scripts -p 'test_behavior_*.py'
```

The second command also uses an existing built web interface and Chrome against a
local test server. It exercises real Display and profile storage interactions,
including ignored writes, failed initial reads, baseline drift, stale-name
collisions, and a lost response after a committed write.
