#!/usr/bin/env python3
"""Install the PlatformIO Core cppcheck tool without the pioarduino name clash.

The pioarduino ESP32 platform declares an optional ``tool-cppcheck`` 2.20.x
package using the same package name as PlatformIO Core's static-analysis tool.
On macOS arm64, that optional package can replace the Core analyzer and make
``pio check`` repeatedly reinstall/segfault. Keep the Core analyzer explicitly
installed before running the check gate.
"""

from __future__ import annotations

import platform
import sys

from platformio.package.manager.tool import ToolPackageManager
from platformio.package.meta import PackageSpec

SPEC = PackageSpec(
    owner="platformio",
    id=8153,
    name="tool-cppcheck",
    requirements="~1.21100.0",
)
DARWIN_ARM64_URL = (
    "https://dl.registry.nm1.platformio.org/tools/57/b0/"
    "fdaea1a2cd44235965ef31dd6bd881167687654e6e653e9cd00c611525ce/"
    "tool-cppcheck-darwin_arm64-1.21100.241030.tar.gz"
)
DARWIN_ARM64_SHA256 = "6de48f52508e37e270c569c3ccf9ab260cba9a351e58c5cb6aa17cb42b0bb60c"


def main() -> int:
    pm = ToolPackageManager()
    for installed in list(pm.get_installed()):
        if installed.metadata.name != SPEC.name:
            continue
        owner = installed.metadata.spec.owner
        if owner == "platformio" and installed.metadata.version in SPEC.requirements:
            continue
        pm.uninstall(installed, skip_dependencies=True)

    pkg = pm.get_package(SPEC)
    if pkg:
        print(f"[toolchain] PlatformIO cppcheck tool OK: {pkg.metadata.version}")
        return 0

    if platform.system() == "Darwin" and platform.machine() == "arm64":
        pkg = pm.install_from_uri(DARWIN_ARM64_URL, SPEC, DARWIN_ARM64_SHA256)
    else:
        pkg = pm.install(SPEC)

    if not pkg:
        print("[toolchain] failed to install PlatformIO cppcheck tool", file=sys.stderr)
        return 1

    print(f"[toolchain] installed PlatformIO cppcheck tool: {pkg.metadata.version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
