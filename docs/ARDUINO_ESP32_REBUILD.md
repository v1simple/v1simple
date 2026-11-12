# Arduino-ESP32 corresponding source and rebuild

V1Simple firmware uses Arduino-ESP32 3.3.12. The release downloads include
`arduino-esp32-source.tar.gz` with the exact patched framework used for the
build, the corresponding V1Simple source tree, and `release_source.json`.
That record identifies the release tag, V1Simple commit, firmware SHA-256,
and verified Arduino inputs and source-tree digest. The archive includes
`LGPL-2.1.txt` alongside the framework directory.
The retained source headers identify the relevant copyright holders and
license terms. See the [upstream Arduino-ESP32 repository](https://github.com/espressif/arduino-esp32/tree/3.3.12).

The V1Simple source implements the build changes in
`scripts/patch_arduino_network_progress.py`,
`scripts/patch_arduino_webserver_body.py` and
`scripts/patch_arduino_fs_overload.py`. They modify `NetworkClient.cpp`,
`Parsing.cpp` and `FS.h` respectively. The archive includes the applied files
and the qualified `tools/pioarduino-build.py` used by the pinned platform.

## Rebuild or relink with this framework

1. Extract the archive and enter its `v1simple/` directory. Install the host
   tools using that tree's `docs/DEVELOPMENT.md` and `config/host_tools.json`.
2. In the extracted `platformio.ini`, add the local framework package to the
   `[env:waveshare-349]` section using its absolute path. This selects the
   archived source in place of the upstream download.

   ```ini
   platform_packages =
       framework-arduinoespressif32 @ file:///absolute/path/framework-arduinoespressif32
   ```

3. Run `npm ci` in `interface/`. From the source tree root, run
   `V1_RELEASE_VERSION=<version> ./build.sh` with the recorded release tag
   without its leading `v`. The application image is
   `.pio/build/waveshare-349/firmware.bin`. The pinned toolchain, ESP-IDF
   libraries and npm dependencies are fetched by the documented build tools.

To relink with your own changes, edit the archived framework before building.
The three listed files already include the V1Simple patches. For a file you
modify, remove its corresponding patch pre-hook in your local `platformio.ini`
and update that file's expected digest in your local
`scripts/verify_esp32s3_framework.py`. The unmodified archived framework uses
the existing pre-hooks and digests as supplied. These steps apply to a local
rebuild from the source download. The release qualification uses the original
pins. A rebuild can have different build metadata or binary bytes.
