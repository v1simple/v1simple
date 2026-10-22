"""PlatformIO pre-hook: compile only a verified, current frontend bundle."""

from pathlib import Path
import sys
from SCons.Script import COMMAND_LINE_TARGETS

Import("env")  # noqa: F821 — PlatformIO SCons global

# Cleanup must work in a fresh checkout. Any accompanying build/upload target
# still validates the bundle; only an exclusively clean invocation skips it.
if not COMMAND_LINE_TARGETS or any(target != "clean" for target in COMMAND_LINE_TARGETS):
    root = Path(env.subst("$PROJECT_DIR"))
    sys.path.insert(0, str(root / "scripts"))
    from generate_web_bundle import generate

    output_dir = Path(env.subst("$BUILD_DIR")) / "include"
    try:
        record = generate(root, output_dir / "embedded_web_assets.generated.h")
    except (OSError, ValueError) as error:
        print(f"[web-bundle] {error}")
        env.Exit(1)
    env.AppendUnique(CPPPATH=[str(output_dir)])
    print(f'[web-bundle] Embedded {record["asset_count"]} current assets ({record["asset_bytes"]} bytes)')
