"""Require and apply the selected non-shipping bench display fault."""

import os


Import("env")  # noqa: F821 - PlatformIO SCons global

raw_fault = os.environ.get("BENCH_FAULT_INJECT", "")
if raw_fault not in {"1", "2", "3"}:
    print("Error: waveshare-349-fault requires BENCH_FAULT_INJECT=1, 2, or 3")
    env.Exit(2)  # noqa: F821 - imported PlatformIO SCons environment

env.Append(CPPDEFINES=[("BENCH_FAULT_INJECT", int(raw_fault))])  # noqa: F821
