# Unattended display and settings bench

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
`restoration-result.json` reports recovery only; it never replaces the qualification
verdict in `result.json`. To reread a complete capture without hardware:

```sh
./behavior-bench.sh analyze --out-dir <existing-private-run-directory>
```

This suite does not qualify RF reception, audio, installation wiring, arbitrary
phone browsers, every possible palette, or interrupted flash recovery. It adds
repeatable physical evidence for the specific saved-color and stuck-alert risks.
Keep release decisions tied to the exact firmware identity and the existing full
CI/release checks; a host-tool test does not qualify a different installed image.

Host regression checks (no DUT):

```sh
"$(./scripts/bench_python.sh --visual)" -m unittest discover -s scripts -p 'test_behavior_*.py'
BEHAVIOR_BROWSER_LIVE_TESTS=1 "$(./scripts/bench_python.sh --browser)" \
  scripts/test_behavior_browser.py
```

The second command uses an existing built web interface and Chrome against a local
test server. It exercises real Save interactions, including ignored writes, failed
initial reads, baseline drift, and a lost response after a committed write.
