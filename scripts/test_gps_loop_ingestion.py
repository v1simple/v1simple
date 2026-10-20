#!/usr/bin/env python3
"""Exercise the production GPS UART drain and drive loop during local adjustments.

Only UART buffering, the disabled OBD source and unrelated runtime collaborators
are modeled. Compile without UNIT_TEST so GPS uses its actual serial input path.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "test/fixtures/gps_loop_ingestion"

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="gps-loop-ingestion-") as temporary:
        executable = Path(temporary) / "recovery"
        compiler = shlex.split(os.environ.get("CXX", "clang++" if shutil.which("clang++") else "c++"))
        subprocess.run(compiler + [
            "-std=c++17", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer", "-g", "-I" + str(FIXTURE), "-I" + str(ROOT),
            str(FIXTURE / "recovery.cpp"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
