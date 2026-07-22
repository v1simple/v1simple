#!/usr/bin/env python3
"""Typed registry for bug-squash HIL case-driver dispatch contracts."""

from __future__ import annotations

from dataclasses import dataclass
from types import MappingProxyType
from typing import Literal, Mapping


CASE_IDS = tuple(f"BSC-{index:02d}" for index in range(2, 15)) + ("BSC-16",)
RUNNER_SOURCE_PATH = "scripts/run_bug_squash_hil.py"
REGISTRY_SOURCE_PATH = "scripts/bug_squash_hil_case_drivers.py"

DriverStatus = Literal["implemented", "blocked"]


@dataclass(frozen=True)
class CaseDriver:
    """Immutable dispatch and evidence contract for one qualification case."""

    case_id: str
    driver_id: str
    entrypoint: str
    status: DriverStatus
    source_path: str
    qualification_blockers: tuple[str, ...]

    @property
    def implemented(self) -> bool:
        return self.status == "implemented"


_BUILD_BLOCKER = "build-generator-provenance-not-authenticated"
_BOARD_BLOCKER = "board-resolution-provenance-not-authenticated"
_FAULT_BLOCKER = "hil-fault-control-not-implemented"
_RIG_BLOCKER = "tracked-rig-adapter-not-implemented"


def _driver(
    case_id: str,
    *,
    status: DriverStatus = "blocked",
    blockers: tuple[str, ...] = (),
) -> CaseDriver:
    suffix = case_id.removeprefix("BSC-")
    return CaseDriver(
        case_id=case_id,
        driver_id=f"bug-squash-{case_id.lower()}-v1",
        entrypoint=f"run_bsc{suffix}_case",
        status=status,
        source_path=RUNNER_SOURCE_PATH,
        qualification_blockers=blockers,
    )


_DRIVERS = (
    _driver(
        "BSC-02",
        status="implemented",
        blockers=(_BUILD_BLOCKER, _BOARD_BLOCKER, _RIG_BLOCKER),
    ),
    _driver(
        "BSC-03",
        status="implemented",
        blockers=(_BUILD_BLOCKER, _BOARD_BLOCKER, _FAULT_BLOCKER),
    ),
    _driver(
        "BSC-04",
        status="implemented",
        blockers=(_BUILD_BLOCKER, _BOARD_BLOCKER, _RIG_BLOCKER),
    ),
    _driver("BSC-05"),
    _driver("BSC-06"),
    _driver("BSC-07"),
    _driver("BSC-08"),
    _driver("BSC-09"),
    _driver("BSC-10"),
    _driver(
        "BSC-11",
        status="implemented",
        blockers=(_BUILD_BLOCKER, _BOARD_BLOCKER, _RIG_BLOCKER),
    ),
    _driver("BSC-12"),
    _driver("BSC-13"),
    _driver("BSC-14"),
    _driver(
        "BSC-16",
        status="implemented",
        blockers=(_BUILD_BLOCKER, _BOARD_BLOCKER, _RIG_BLOCKER),
    ),
)


class CaseDriverContractError(ValueError):
    """The tracked registry cannot be used safely."""


def _validate_registry(drivers: tuple[CaseDriver, ...]) -> None:
    if tuple(driver.case_id for driver in drivers) != CASE_IDS:
        raise CaseDriverContractError("case-driver registry does not match the pinned case set")
    if len({driver.driver_id for driver in drivers}) != len(drivers):
        raise CaseDriverContractError("case-driver registry contains duplicate driver identities")
    for driver in drivers:
        suffix = driver.case_id.removeprefix("BSC-")
        if driver.driver_id != f"bug-squash-{driver.case_id.lower()}-v1":
            raise CaseDriverContractError("case-driver identity is invalid")
        if driver.entrypoint != f"run_bsc{suffix}_case":
            raise CaseDriverContractError("case-driver entrypoint is invalid")
        if driver.status not in {"implemented", "blocked"}:
            raise CaseDriverContractError("case-driver status is invalid")
        if driver.source_path != RUNNER_SOURCE_PATH:
            raise CaseDriverContractError("case-driver source path is invalid")
        if not driver.implemented and driver.qualification_blockers:
            raise CaseDriverContractError("blocked case driver cannot claim qualification blockers")
        if len(set(driver.qualification_blockers)) != len(driver.qualification_blockers):
            raise CaseDriverContractError("case-driver blockers must be unique")


_validate_registry(_DRIVERS)
DRIVERS: tuple[CaseDriver, ...] = _DRIVERS
DRIVER_BY_CASE: Mapping[str, CaseDriver] = MappingProxyType(
    {driver.case_id: driver for driver in DRIVERS}
)


def get_case_driver(case_id: str) -> CaseDriver:
    """Resolve exactly one tracked descriptor for a pinned case ID."""

    try:
        return DRIVER_BY_CASE[case_id]
    except KeyError as exc:
        raise CaseDriverContractError("case ID is not present in the tracked registry") from exc


def implemented_entrypoints() -> tuple[str, ...]:
    """Return the exact entrypoint set the runner must provide."""

    return tuple(driver.entrypoint for driver in DRIVERS if driver.implemented)
