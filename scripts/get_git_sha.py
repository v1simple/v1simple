#!/usr/bin/env python3
"""Inject GIT_SHA only into build_metadata.cpp so incremental builds stay cached.

Used as a PlatformIO extra_script (pre:).  Applies a per-source CPPDEFINE
so the macro never touches any other translation unit.
"""
import subprocess, os

Import("env")  # noqa: F821  — PlatformIO SCons global

try:
    sha = subprocess.check_output(
        ["git", "rev-parse", "--short=7", "HEAD"],
        stderr=subprocess.DEVNULL,
    ).decode().strip()
except Exception:
    sha = "unknown"

def inject_git_sha(env, node):
    """SCons per-source callback: add -DGIT_SHA only for build_metadata.cpp."""
    src = str(node.srcnode())
    if os.path.basename(src) == "build_metadata.cpp":
        clone = env.Clone()
        clone.Append(CPPDEFINES=[("GIT_SHA", '\\"' + sha + '\\"')])
        return clone.Object(node)
    return node

env.AddBuildMiddleware(inject_git_sha)
