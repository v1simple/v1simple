"""Keep PlatformIO uploads aligned with the custom flash map.

The production partition table places the factory app at 0x20000 and extends NVS
through 0x10fff. PlatformIO's ESP32 defaults write the app at 0x10000 and also
add Arduino's boot_app0 image at 0xe000; both overlap this project's NVS/app
layout. This script runs before and after the platform builder, and also patches
the resolved upload command immediately before upload, so uploads match
partitions_v1.csv.
"""

import re

from SCons.Script import COMMAND_LINE_TARGETS  # pylint: disable=import-error

Import("env")  # pylint: disable=undefined-variable

APP_OFFSET = "0x20000"
BAD_DEFAULT_OFFSETS = ("0x10000", "0x00010000")
BOOT_APP0_CMD_RE = re.compile(r"\s+0x0*e000\s+\S*boot_app0\.bin")


def strip_boot_app0_flags(flags):
    """Remove Arduino's OTA boot_app0 write; it overlaps this project's NVS."""
    stripped = []
    index = 0
    while index < len(flags):
        if index + 1 < len(flags) and str(flags[index + 1]).endswith("boot_app0.bin"):
            index += 2
            continue
        stripped.append(flags[index])
        index += 1
    return stripped


def patch_upload_command(command):
    """Replace the default app offset and remove boot_app0 from a shell command."""
    patched = BOOT_APP0_CMD_RE.sub("", command)
    for bad_offset in BAD_DEFAULT_OFFSETS:
        patched = patched.replace(f" {bad_offset} ", f" {APP_OFFSET} ")
    return patched


def force_flash_layout(build_env):
    """Update all known upload-layout locations in a PlatformIO SCons env."""
    build_env.BoardConfig().update("upload.offset_address", APP_OFFSET)
    build_env.Replace(ESP32_APP_OFFSET=APP_OFFSET)

    integration_data = build_env.get("INTEGRATION_EXTRA_DATA", {}) or {}
    integration_data.update({"application_offset": APP_OFFSET})
    build_env.Replace(INTEGRATION_EXTRA_DATA=integration_data)

    flags = build_env.get("UPLOADERFLAGS", []) or []
    if flags:
        patched_flags = [
            APP_OFFSET if str(flag).lower() in BAD_DEFAULT_OFFSETS else flag
            for flag in strip_boot_app0_flags(flags)
        ]
        build_env.Replace(UPLOADERFLAGS=patched_flags)

    upload_cmd = build_env.get("UPLOADCMD")
    if isinstance(upload_cmd, str):
        patched = patch_upload_command(upload_cmd)
        if patched != upload_cmd:
            build_env.Replace(UPLOADCMD=patched)


def force_before_upload(target, source, env):
    """Patch late-reset upload variables immediately before the upload action."""
    # This pre-action runs before PlatformIO's BeforeUpload() autodetects the
    # serial port, so do not subst() the whole command here.  Keep $UPLOAD_PORT
    # and $SOURCE symbolic, and only patch the offset/extra-image variables.
    force_flash_layout(env)
    upload_cmd = env.get("UPLOADCMD")
    if isinstance(upload_cmd, str):
        env.Replace(UPLOADCMD=patch_upload_command(upload_cmd))
    print(
        f"[UploadOffset] pre-upload app image offset forced to {APP_OFFSET}; "
        "boot_app0 skipped"
    )


force_flash_layout(env)

if any(target in ("upload", "erase_upload") for target in COMMAND_LINE_TARGETS):
    print(f"[UploadOffset] app image offset forced to {APP_OFFSET}; boot_app0 skipped")
    if not env.get("APP_UPLOAD_OFFSET_PREACTION_REGISTERED"):
        env.Replace(APP_UPLOAD_OFFSET_PREACTION_REGISTERED=True)
        env.AddPreAction("upload", force_before_upload)
        env.AddPreAction("erase_upload", force_before_upload)
