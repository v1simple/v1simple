#!/usr/bin/env python3
"""Inject build metadata supplied outside the tracked source tree.

GIT_SHA is applied only to build_metadata.cpp so incremental builds stay
cached. Release may also supply V1_RELEASE_VERSION; that selected semantic
version must reach every translation unit that consumes FIRMWARE_VERSION.
"""
import os
import re
import subprocess

Import("env")  # noqa: F821  — PlatformIO SCons global

release_version = os.environ.get("V1_RELEASE_VERSION", "")
if release_version:
    if re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", release_version) is None:
        print(f"Error: invalid V1_RELEASE_VERSION: {release_version!r}")
        env.Exit(2)
    env.Append(CPPDEFINES=[("FIRMWARE_VERSION", '\\"' + release_version + '\\"')])

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
