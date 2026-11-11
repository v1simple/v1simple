# Changelog

This is the short version of changes users and integrators may notice. Git and
GitHub Releases retain the complete commit history.

## v2.4.4

- Prevented new profile saves from overwriting an existing profile, including
  when the saved-profile list is unavailable.
- Preserved acknowledged profile edits when an editor is closed or reopened
  during a save.
- Reloaded saved display and Auto-Push settings after lost save replies while
  preserving later draft edits.
- Preserved existing settings when memory is insufficient to prepare a rollback.
- Protected saved Wi-Fi slots when a save or readback is interrupted, and kept
  the prior network priorities when the new primary name cannot be staged.
- Improved touchscreen slider tracking during a held drag and corrected
  release detection.
- Kept zero-volume warnings updating during quiet periods on a connected detector.
- Retried briefly refused Bluetooth proxy notifications and preserved replies
  and phone commands arriving during concurrent proxy traffic.
- Kept the detector's Bluetooth pairing across ordinary disconnects.
- Let the preserving installer recover from a stalled download and retry before
  writing to the device.
- Documented how the three USB profile slot indices map to their web interface
  labels.
- Added automated profile, display and audio bench checks, including warning
  cadence verification against the original camera frames.

## v2.4.3 — 2026-11-03

- Added an offline first-use guide to the maintenance interface and installer.
- Exposed observed settings-save and SD-backup status so pending work and
  recovery results are visible.
- Reloaded device settings after an uncertain restore response before allowing
  another Save, and serviced the watchdog between restored profile transactions.
- Rejected Auto-Push saves when a complete slot snapshot is unavailable,
  preserving the existing configuration.
- Prevented lost alert input from authorizing an In-the-Box automatic mute.
- Preserved committed profile replacements when a later settings read fails.
- Rejected profile names that share an SD filename, preserving existing
  profiles and slot assignments when a save or import is refused.
- Blocked Bluetooth startup after incomplete bond-table reads, preserving the
  SD bond backup for retry, and corrected empty-table initialization.
- Rejected queued proxy notifications from retired detector or phone sessions.
- Kept operating-mode edits disabled until saved settings are read, and
  reloaded the quiet-screen setting after a lost save response.
- Corrected GPS detection timing when its start timestamp is zero.
- Aligned generated factory images with the application partition and kept
  filesystem staging limited to explicit filesystem builds and uploads.
- Removed unused state, obsolete adapters and duplicate configuration paths
  across firmware, the maintenance interface and replay tools.
- Verified declared camera capture identities, file hashes and recorded replay
  expectations before reading replay fields, with explicit handling for older
  camera recordings.

## v2.4.1 — 2026-10-16

- Preserved newer profile drafts and selections when an earlier Edit or Copy
  request finishes later.
- Cancelled a deferred In-the-Box unmute when the remaining alerts are on bands
  whose inside-unmute option is off.
- Corrected handling of missing GPS speed and stale fixes so they cannot keep
  low-speed muting active.
- Preserved alert receipt times through BLE buffering and recovered from
  incomplete interleaved packets without blocking later traffic.
- Fixed mute and volume-fade ownership and avoided repeated secondary voice
  announcements.
- Improved Wi-Fi credential rollback and reserved event-log space for recovery
  after reboot.
- Kept profile Apply feedback visible, shared operation tracking with Auto-Push,
  and avoided rewriting unchanged profiles.
- Fixed replay decoding of checksum-free and indexed proxy captures.
- Updated the maintenance frontend, firmware toolchain, graphics library, and
  web installer dependencies; improved Windows toolchain discovery.

## v2.4.0 — 2026-10-02

- Simplified profile controls and clarified detector display settings, including
  the Bluetooth indicator in dark mode.
- Added editable In-the-Box frequency ranges with optional outside muting and
  inside unmuting. Both actions default off, and alerts remain visible.
- Fixed a sweep-read retry that could block custom-frequency profile application
  by treating stale definitions as a complete response.

## v2.3.1 — 2026-09-22

- Replay evidence now captures presentation settings before serial access and
  binds them to the recorded detector state and camera run.
- A missing maintenance snapshot is reported as incomplete evidence instead of
  a firmware failure, and normal-mode replay no longer probes maintenance WiFi.
- The web installer now discloses its cookie-free visit and install-choice
  counting.

## v2.3.0 — 2026-09-21

- Fixed alert presentation edge cases involving Photo, Ku, laser, muting,
  rollover, frequency display, and voice announcements.
- Hardened profile-owned detector settings, custom frequencies, Auto-Push
  recovery, and maintenance requests against partial or stale state.
- Improved release reproducibility, clean-Linux validation, bench progress, and
  camera capture on current macOS versions.

Older release details remain available in GitHub Releases.
