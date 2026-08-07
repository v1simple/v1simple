"""PlatformIO pre-build hook for the opt-in linked native-test pilot."""

from __future__ import annotations

import os
import sys
from pathlib import Path

Import("env")  # type: ignore[name-defined]  # noqa: F821

project_root = Path(env.subst("$PROJECT_DIR")).resolve()  # type: ignore[name-defined]  # noqa: F821
sys.path.insert(0, str(project_root / "scripts"))
from native_test_source_manifest import (
    LINKED_NATIVE_TEST_PILOT_ENV,
    linked_test_spec,
    validate_manifest,
)

linked_pilot = os.environ.get(LINKED_NATIVE_TEST_PILOT_ENV)

if linked_pilot is not None:
    if linked_pilot != "1":
        sys.stderr.write(
            f"Error: {LINKED_NATIVE_TEST_PILOT_ENV} must be exactly '1' when set.\n"
        )
        env.Exit(2)  # type: ignore[name-defined]  # noqa: F821

    test_name = env.get("PIOTEST_RUNNING_NAME")  # type: ignore[name-defined]  # noqa: F821
    if not test_name:
        sys.stderr.write(
            f"Error: {LINKED_NATIVE_TEST_PILOT_ENV}=1 requires one exact "
            "PIOTEST_RUNNING_NAME.\n"
        )
        env.Exit(2)  # type: ignore[name-defined]  # noqa: F821

    try:
        validate_manifest(project_root)
        spec = linked_test_spec(str(test_name))
    except ValueError as exc:
        sys.stderr.write(f"Error: linked native-test configuration rejected: {exc}\n")
        env.Exit(2)  # type: ignore[name-defined]  # noqa: F821

    env.AppendUnique(CPPDEFINES=[spec.define])  # type: ignore[name-defined]  # noqa: F821

    # Extra scripts run before PlatformIO's project-library builder creates the
    # environment used for ordinary test sources.  Build linked sources from a
    # clone with the same project and installed-library include roots so they do
    # not depend on an amalgamating test translation unit to inherit them.
    linked_env = env.Clone()  # type: ignore[name-defined]  # noqa: F821
    linked_env.ProcessFlags(linked_env.get("BUILD_FLAGS"))
    linked_env.PrependUnique(
        CPPPATH=[
            linked_env.subst("$PROJECT_INCLUDE_DIR"),
            linked_env.subst("$PROJECT_SRC_DIR"),
            *[
                linked_env.subst(
                    os.path.join("$PROJECT_LIBDEPS_DIR", "$PIOENV", include_dir)
                )
                for include_dir in spec.library_include_dirs
            ],
        ]
    )

    source_filter = [f"+<{source}>" for source in spec.sources]
    linked_env.BuildSources(
        os.path.join("$BUILD_DIR", "linked-production", str(test_name)),
        "$PROJECT_DIR",
        source_filter,
    )
