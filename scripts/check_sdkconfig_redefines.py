#!/usr/bin/env python3
"""
CI guard: detect when platformio.ini -D CONFIG_* flags are inert because
the framework's prebuilt sdkconfig.h #defines the same symbol unconditionally.

Background — the platformio.ini comment block at lines 64-74 documents
that pioarduino's prebuilt sdkconfig.h unconditionally defines
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE and CONFIG_BT_NIMBLE_MAX_CONNECTIONS,
defeating any -D override (last #define wins in GCC). The same pattern
applies to every CONFIG_* symbol the framework header emits — including
CONFIG_BT_CTRL_AGC_RECORRECT_EN (review H1).

This guard parses the framework's sdkconfig.h (located via the PIO
package layout) and reports any platformio.ini -D CONFIG_* flag that
is also defined by the framework header. Exit code is 0 with a WARNING
when redefinition is detected, so the build doesn't break, but the
warning is loud enough to force a maintainer to either:

  1. Confirm via `gcc -E` that the -D actually lands (runtime patch
     ordering may differ from the simple "last wins" model), and update
     the comment in platformio.ini accordingly, or
  2. Remove the inert -D flag and apply the desired effect a different
     way (software offset, framework-config patch, etc.).

Exit codes:
  0 — clean OR redefinition detected (with WARNING)
  1 — internal error parsing the build environment
  2 — unexpected failure
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parent.parent
PLATFORMIO_INI = REPO_ROOT / "platformio.ini"

# Symbols we explicitly care about. Anything else is informational only.
CRITICAL_SYMBOLS = {
    "CONFIG_BT_CTRL_AGC_RECORRECT_EN",
    "CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE",
    "CONFIG_BT_NIMBLE_MAX_CONNECTIONS",
}

# Where pioarduino installs the prebuilt framework headers. The exact
# subdirectory depends on the platform release; we search a few known
# layouts and fall back to a glob.
PIO_CORE_DIR = Path(os.environ.get("PLATFORMIO_CORE_DIR", str(Path.home() / ".platformio")))
PIO_PACKAGES_DIR = PIO_CORE_DIR / "packages"
FRAMEWORK_LIBS_DIR = PIO_PACKAGES_DIR / "framework-arduinoespressif32-libs"


def _parse_platformio_sections() -> dict[str, dict[str, str]]:
    """Parse simple key/value data from platformio.ini sections."""
    sections: dict[str, dict[str, str]] = {}
    current_section: str | None = None

    if not PLATFORMIO_INI.exists():
        return sections

    for raw_line in PLATFORMIO_INI.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if not line or line.startswith((";", "#")):
            continue
        if line.startswith("[") and line.endswith("]"):
            current_section = line[1:-1].strip()
            sections.setdefault(current_section, {})
            continue
        if current_section is None or "=" not in line:
            continue
        key, value = line.split("=", 1)
        sections[current_section][key.strip()] = value.strip()

    return sections


def _normalize_board_to_chip(board: str) -> str | None:
    board_lc = board.lower()
    if "esp32-s3" in board_lc or "esp32s3" in board_lc:
        return "esp32s3"
    if "esp32-s2" in board_lc or "esp32s2" in board_lc:
        return "esp32s2"
    if "esp32-c6" in board_lc or "esp32c6" in board_lc:
        return "esp32c6"
    if "esp32-c5" in board_lc or "esp32c5" in board_lc:
        return "esp32c5"
    if "esp32-c3" in board_lc or "esp32c3" in board_lc:
        return "esp32c3"
    if "esp32-h2" in board_lc or "esp32h2" in board_lc:
        return "esp32h2"
    if "esp32-p4" in board_lc or "esp32p4" in board_lc:
        return "esp32p4"
    if "esp32" in board_lc:
        return "esp32"
    return None


def _target_sdkconfig_path() -> Path | None:
    sections = _parse_platformio_sections()
    platformio_section = sections.get("platformio", {})
    default_envs = platformio_section.get("default_envs", "")
    target_env = default_envs.split(",", 1)[0].strip() or "waveshare-349"
    env_section = sections.get(f"env:{target_env}", {})
    chip = _normalize_board_to_chip(env_section.get("board", ""))
    memory_type = env_section.get("board_build.arduino.memory_type", "")

    if not chip or not memory_type:
        return None

    candidate = FRAMEWORK_LIBS_DIR / chip / memory_type / "include" / "sdkconfig.h"
    return candidate if candidate.exists() else None


def _platformio_d_flags() -> dict[str, str]:
    """Extract `-D CONFIG_*=<value>` flags from platformio.ini."""
    pat = re.compile(r"-D\s+(CONFIG_[A-Z0-9_]+)\s*=\s*(\S+)")
    flags: dict[str, str] = {}
    if not PLATFORMIO_INI.exists():
        return flags
    text = PLATFORMIO_INI.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        s = line.strip()
        if s.startswith(";") or s.startswith("#"):
            continue
        for m in pat.finditer(s):
            # Last assignment wins — matches what GCC sees.
            flags[m.group(1)] = m.group(2)
    return flags


def _candidate_sdkconfig_paths() -> Iterable[Path]:
    preferred = _target_sdkconfig_path()
    if preferred is not None:
        yield preferred

    # Fallback: search the package tree, but keep the preferred path first.
    if PIO_PACKAGES_DIR.is_dir():
        for path in PIO_PACKAGES_DIR.rglob("sdkconfig.h"):
            if preferred is not None and path == preferred:
                continue
            yield path


def _framework_defines(sdk_path: Path) -> dict[str, str]:
    """Parse `#define CONFIG_X value` lines from the framework's sdkconfig.h."""
    defines: dict[str, str] = {}
    pat = re.compile(r"^\s*#define\s+(CONFIG_[A-Z0-9_]+)\s+(.+?)\s*$")
    try:
        with sdk_path.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = pat.match(line)
                if m:
                    defines[m.group(1)] = m.group(2)
    except OSError:
        pass
    return defines


def main() -> int:
    flags = _platformio_d_flags()
    if not flags:
        print("check_sdkconfig_redefines: no -D CONFIG_* flags in platformio.ini")
        return 0

    sdk_paths = list(_candidate_sdkconfig_paths())
    if not sdk_paths:
        # Framework not installed locally — typical in fresh CI before pio
        # has populated ~/.platformio. Skip with a note.
        print("check_sdkconfig_redefines: framework sdkconfig.h not found "
              "(skip — framework probably not installed yet)")
        for sym in sorted(set(flags) & CRITICAL_SYMBOLS):
            print(f"  reminder: {sym}={flags[sym]} is on the platformio.ini "
                  f"command line; verify it lands at runtime.")
        return 0

    # Use the first sdkconfig.h found — for the targeted board they should
    # all agree, but report which one was consulted for transparency.
    sdk_path = sdk_paths[0]
    framework = _framework_defines(sdk_path)

    try:
        display_path: Path | str = sdk_path.relative_to(Path.home())
    except ValueError:
        display_path = sdk_path
    print(f"check_sdkconfig_redefines: comparing platformio.ini -D flags "
          f"against {display_path}")

    redefined: list[tuple[str, str, str]] = []
    for sym, cmdline_value in flags.items():
        fw_value = framework.get(sym)
        if fw_value is None:
            continue
        if fw_value.strip() == cmdline_value.strip():
            # Same value — harmless, the -D is redundant but not inert.
            continue
        redefined.append((sym, cmdline_value, fw_value))

    if not redefined:
        print("check_sdkconfig_redefines: no inert -D flags detected")
        return 0

    print()
    print("WARNING: the following platformio.ini -D flags are likely INERT —")
    print("the framework's prebuilt sdkconfig.h unconditionally redefines")
    print("them with a different value:")
    print()
    for sym, cmd, fw in redefined:
        marker = "  *" if sym in CRITICAL_SYMBOLS else "   "
        print(f"{marker} {sym}")
        print(f"      platformio.ini -D : {cmd}")
        print(f"      framework header  : {fw}")
    print()
    print("Critical symbols (marked with *) MUST be verified with `gcc -E`")
    print("on a real build. If the override is genuinely inert, remove the")
    print("-D flag and apply the effect a different way (software offset,")
    print("sdkconfig.defaults patch, etc.).")
    print()
    # Emit warning, do NOT fail — verification is a build-system audit,
    # not a code regression.
    return 0


if __name__ == "__main__":
    sys.exit(main())
