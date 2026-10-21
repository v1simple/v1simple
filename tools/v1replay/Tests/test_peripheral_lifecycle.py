#!/usr/bin/env python3
"""Run real peripheral owner callbacks with a software-only CoreBluetooth module.

Uses installed Xcode tools only. Production sources are compiled unchanged;
the fake module is visible only to this temporary build, never to SwiftPM.
"""
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    package = Path(__file__).resolve().parents[1]
    support = package / "Tests" / "TestSupport" / "PeripheralLifecycle"
    sources = package / "Sources" / "v1replay"
    platform = Path(subprocess.check_output(
        ["xcrun", "--sdk", "macosx", "--show-sdk-platform-path"], text=True
    ).strip())
    frameworks = platform / "Developer" / "Library" / "Frameworks"
    test_libraries = platform / "Developer" / "usr" / "lib"
    with tempfile.TemporaryDirectory(prefix="v1replay-peripheral-tests-") as directory:
        build = Path(directory)
        env = os.environ.copy()
        env["CLANG_MODULE_CACHE_PATH"] = str(build / "clang-cache")
        compiler = ["xcrun", "swiftc", "-module-cache-path", str(build / "swift-cache")]
        subprocess.run(compiler + ["-emit-module", "-emit-library", "-module-name", "CoreBluetooth",
                       str(support / "CoreBluetooth.swift"), "-o", str(build / "libCoreBluetooth.dylib")],
                       cwd=build, env=env, check=True)
        owners = ["Encounter", "Peripheral", "Player", "PlaybackPacketPlan", "ReplayEvidence",
                  "ReplayStimulusEvent", "Session", "V1Protocol"]
        subprocess.run(compiler + ["-I", str(build), "-L", str(build), "-lCoreBluetooth",
                       "-I", str(test_libraries), "-L", str(test_libraries),
                       "-Xlinker", "-rpath", "-Xlinker", str(test_libraries),
                       "-F", str(frameworks), "-Xlinker", "-rpath", "-Xlinker", str(frameworks),
                       "-Xlinker", "-rpath", "-Xlinker", str(build)] +
                       [str(sources / (name + ".swift")) for name in owners] +
                       [str(support / "main.swift"), "-o", str(build / "peripheral-tests")],
                       cwd=build, env=env, check=True)
        subprocess.run([str(build / "peripheral-tests")], cwd=build, env=env, check=True)


if __name__ == "__main__":
    main()
