# Installation and recovery

For the short path, start with the [README](../README.md) or
[browser installer](https://www.v1simple.com/). This page keeps the hardware,
upgrade, and recovery details in one place.

## Hardware and safety

- Waveshare ESP32-S3-Touch-LCD-3.49, board revision V1 or V2
- Valentine One Gen 2 with BLE enabled

### Board revision — read before ordering

Waveshare discontinued the V1 board and says shipments switched to V2 after
2026-06-08. The firmware supports both revisions with automatic detection and
routing at boot:

| Board | Backlight | Panel reset |
|---|---|---|
| V1 | GPIO8 | GPIO21 |
| V2 | GPIO42 | EXIO5 |

Detection samples GPIO21 and EXIO5 as complementary reset/TE candidates before
enabling the display outputs. If the revision cannot be identified safely, the
firmware leaves the swapped candidates passive and the screen dark.

Identify the revision before flashing:

| | V1 | V2 |
|---|---|---|
| PCB silkscreen | Rev1.0 | **Rev1.1** |
| QC sticker on the case | none | **V2** |

The revision numbering is confusing: silkscreen `Rev1.1` is the *newer* V2
board. Waveshare documents the revision change at
<https://docs.waveshare.com/ESP32-S3-Touch-LCD-3.49>.

### Safety

Use the board's USB connection for the documented bench, flash, and test path.
The `esp32-s3-car-install` build only changes firmware shutdown behavior; it is
not vehicle-power wiring guidance. Any vehicle installation needs a separately
verified, protected power supply and wiring.

The cased board is sold with and without a lithium cell. A lithium battery left
in a parked vehicle can exceed its rated temperature range. Choosing a
no-battery variant for a permanently mounted install avoids that; this project
does not verify thermal behavior in any configuration.

## Install and configure

Follow the browser installer's preparation and flashing steps, then reset the
board.

On a normal boot, the firmware starts scanning for the detector. Hold and
release **BOOT** after about four seconds to reboot into maintenance mode. Join
the default `V1-Simple` network with `setupv1simple`, open
`http://192.168.35.5`, and change that default password during first setup.
Use your saved network name and password instead if they were changed or
restored from SD.

The web page is available only in maintenance mode. Click **Exit** in its top
bar to restart normal display operation. The WiFi connection closes as the
device leaves maintenance mode.

For quick brightness and volume adjustments during normal operation, briefly
press **BOOT** to open the touchscreen sliders; press it again to save and close.
A live alert can close the sliders so the alert remains visible.

## Upgrading existing device data

- **Update and preserve data:** updates firmware and the maintenance web
  interface together while preserving settings, WiFi networks, profiles, and
  stored audio. Restart after the transfer, then reopen maintenance mode.
- **Fresh install:** installs firmware and interface together, but erases saved
  settings, WiFi networks, and internal fallback profiles, even with
  **Erase device** off. SD-card contents remain intact; a valid SD backup can restore
  saved settings on boot. Read the installer's warnings before choosing this path.

A normal preserving upgrade does not require deleting profiles, resetting
settings, or erasing storage. Supported older profile and Auto-Push data is
converted atomically on boot. If an older catalog has more than 10 profiles,
delete unused profiles from the maintenance Profiles page and restart so
migration can finish—do not factory-reset the device.

The web interface is bundled in the application image and served directly from
firmware; updating it does not rewrite LittleFS. The preserving installer writes
only the application partition and leaves the bootloader, partition table, NVS,
LittleFS, and SD card intact. This supports existing V1Simple installations with
the current partition layout. A release that changes that layout needs explicit
upgrade instructions; this installer does not migrate partition tables.

If power or the USB connection is lost during an update, the device may not
start until the update is retried. There is one application slot and no automatic
rollback. Reconnect in bootloader mode by holding **Power + GEAR** while
connecting USB-C, then choose **Update and preserve data** again. Do not choose
**Fresh install** to recover: it erases saved device data. After the retry
completes, restart, reopen maintenance mode, and check your settings and profiles.

See [USB profile backup and restore](USB_PROFILES.md#upgrading-existing-device-data)
for migration and recovery details. Storage migration does not prove that the
detector received or applied a profile.
