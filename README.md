# V1Simple

A free, customizable touchscreen for your **Valentine One Gen 2**. See alerts,
save detector profiles, and adjust colors and audio. V1Simple connects over
Bluetooth and runs on the **Waveshare ESP32-S3-Touch-LCD-3.49**, revision V1 or V2.

**[Install V1Simple →](https://www.v1simple.com/)** — use a desktop browser and a
USB-C data cable. No programming or build tools needed.

## Get started

1. **Install.** Open the [browser installer](https://www.v1simple.com/) and follow
   its preparation steps. For a new device, choose **Erase and fresh install**.
   For an existing V1Simple device, choose **Update and preserve data**.
2. **Start the display.** Press **RESET** after installation. Power on your V1
   Gen 2 with Bluetooth enabled; V1Simple scans for it automatically.
3. **Customize.** Hold **BOOT** for about four seconds, then release. The device
   restarts into maintenance mode. Join **V1-Simple** WiFi with password
   **setupv1simple**, then open **[http://192.168.35.5](http://192.168.35.5)**.
4. **Return to the display.** Click **Exit** at the top of the web page. The
   device restarts into normal operation and disconnects the maintenance WiFi.

Use your saved WiFi credentials if changed or restored from SD. If your phone
or computer says this network has no internet, stay connected. Change the default
password in **Settings** during first setup. The web page is only available in
maintenance mode.

For quick brightness and volume changes, briefly press **BOOT**, use the
screen sliders, then press **BOOT** again to save and close.

## Updating

**Update and preserve data** updates both firmware and the web interface while
keeping settings, WiFi networks, profiles, and stored audio on supported existing
installations. **Fresh install erases internal saved data**, even with the
optional erase checkbox off. SD-card contents remain intact.

If an update is interrupted, retry **Update and preserve data**. See
[installation and recovery](docs/INSTALLATION.md) for the button sequence,
board identification, and upgrade details.

## Before mounting in a vehicle

Start with USB power. Vehicle installations need a verified, protected supply
and wiring. Consider the no-battery board: a lithium cell in a parked vehicle
can overheat. This project does not verify vehicle power or thermal behavior.

[Build and test](docs/DEVELOPMENT.md) ·
[USB profile backup](docs/USB_PROFILES.md) ·
[Security](SECURITY.md)

MIT licensed; see [LICENSE](LICENSE) and [third-party notices](THIRD_PARTY_NOTICES.md).
Independent project; not affiliated with or endorsed by Valentine Research, Inc.
