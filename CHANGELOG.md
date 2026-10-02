# Changelog

This is the short version of changes users and integrators may notice. Git and
GitHub Releases retain the complete commit history.

## v2.4.0 — Unreleased

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
