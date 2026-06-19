# Security Policy

## Reporting a vulnerability

Please report security issues **privately** — do not open a public issue for
anything exploitable.

- Preferred: GitHub **private vulnerability reporting** — use *"Report a
  vulnerability"* under the repository's **Security** tab.
- Please include the affected firmware version (`FIRMWARE_VERSION` in
  `include/config.h`), the hardware/board, reproduction steps, and impact.

This is a volunteer, community-run project. We try to acknowledge reports
promptly but cannot commit to a fixed response SLA.

## Scope

v1simple is a hobbyist accessory for a Valentine 1 radar detector. The most
security-relevant surfaces are:

- The **maintenance-mode WiFi access point** and its web UI / `/api/*`
  endpoints. Note that the maintenance interface is only brought up by a
  deliberate maintenance gesture and is **not** running during normal drive
  runtime.
- The **BLE proxy** that mediates between the V1 and companion phone apps.

Out of scope: physical attacks requiring possession of the device, and issues
in third-party dependencies (report those upstream).

## Supported versions

Only the most recent release on `main` is supported.
