#!/usr/bin/env python3
"""Immutable trust contracts for tracked bug-squash HIL rig adapters.

The registry describes the boundary that a future physical adapter must cross.
No adapter is executable until its descriptor is changed to ``implemented``
with a tracked source and entrypoint.  Raw artifact hashes are deliberately not
part of this registry or the adapter response: the runner owns those hashes.
"""

from __future__ import annotations

from dataclasses import dataclass
from types import MappingProxyType
import re
from typing import Literal, Mapping


ADAPTER_PROTOCOL_VERSION = 1
CASE_IDS = tuple(f"BSC-{index:02d}" for index in range(2, 15)) + ("BSC-16",)
REGISTRY_SOURCE_PATH = "scripts/bug_squash_hil_rig_adapters.py"

AdapterStatus = Literal["unavailable", "implemented"]
BuildKind = Literal["hil-fault", "production", "car-production"]

SAFE_SLUG_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")
SAFE_FILENAME_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,127}$")
SAFE_SOURCE_RE = re.compile(r"^scripts/bug_squash_hil_[a-z0-9_]+\.py$")
SAFE_ENTRYPOINT_RE = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
MAX_RAW_ARTIFACT_BYTES = 32 * 1024 * 1024


class RigAdapterContractError(ValueError):
    """The tracked rig-adapter contract cannot be used safely."""


@dataclass(frozen=True)
class RawArtifactContract:
    role: str
    filename: str
    maximum_bytes: int


@dataclass(frozen=True)
class AdapterRoleContract:
    role_id: str
    build_kind: BuildKind
    firmware_environment: str
    raw_artifacts: tuple[RawArtifactContract, ...]


@dataclass(frozen=True)
class RigAdapter:
    case_id: str
    adapter_id: str
    protocol_version: int
    status: AdapterStatus
    source_path: str | None
    entrypoint: str | None
    minimum_runs: int
    required_dut_capabilities: tuple[str, ...]
    required_rig_capabilities: tuple[str, ...]
    roles: tuple[AdapterRoleContract, ...]

    @property
    def implemented(self) -> bool:
        return self.status == "implemented"


RAW_ARTIFACTS = (
    RawArtifactContract("adapter-transcript", "adapter-transcript.jsonl", 4 * 1024 * 1024),
    RawArtifactContract("case-observation", "case-observation.json", 2 * 1024 * 1024),
    RawArtifactContract("firmware-build", "firmware-build.json", 1024 * 1024),
    RawArtifactContract("safety-summary", "safety-summary.json", 1024 * 1024),
    RawArtifactContract("serial-log", "serial.log", 16 * 1024 * 1024),
)
BSC12_RAW_ARTIFACTS = (
    RawArtifactContract("firmware-build", "firmware-build.json", 1024 * 1024),
    RawArtifactContract("persistence-after", "persistence-after.json", 512 * 1024),
    RawArtifactContract("persistence-before", "persistence-before.json", 512 * 1024),
    RawArtifactContract("power-reset-trace", "power-reset-trace.json", 2 * 1024 * 1024),
    RawArtifactContract("serial-log", "serial.log", 16 * 1024 * 1024),
    RawArtifactContract("shutdown-timeline", "shutdown-timeline.json", 2 * 1024 * 1024),
    RawArtifactContract("wake-input-trace", "wake-input-trace.json", 2 * 1024 * 1024),
)
BSC16_RAW_ARTIFACTS = (
    RawArtifactContract("build-evidence", "build-evidence.json", 1024 * 1024),
    RawArtifactContract("logic-analyzer", "logic-analyzer-capture", 32 * 1024 * 1024),
    RawArtifactContract("poweroff-log", "poweroff.log", 2 * 1024 * 1024),
    RawArtifactContract("serial-log", "serial.log", 16 * 1024 * 1024),
    RawArtifactContract("source-transitions", "source-transitions.ndjson", 2 * 1024 * 1024),
)

BSC07_RAW_ARTIFACTS = (
    RawArtifactContract("ap-traffic", "ap-traffic.json", 2 * 1024 * 1024),
    RawArtifactContract("firmware-build", "firmware-build.json", 1024 * 1024),
    RawArtifactContract("power-timeline", "power-timeline.json", 2 * 1024 * 1024),
    RawArtifactContract("reset-summary", "reset-summary.json", 1024 * 1024),
    RawArtifactContract("serial-log", "serial.log", 16 * 1024 * 1024),
    RawArtifactContract("ui-health", "ui-health.json", 2 * 1024 * 1024),
)

BSC08_RAW_ARTIFACTS = (
    RawArtifactContract("adapter-transcript", "adapter-transcript.jsonl", 4 * 1024 * 1024),
    RawArtifactContract("firmware-build", "firmware-build.json", 1024 * 1024),
    RawArtifactContract("proxy-peer-receipts", "proxy-peer-receipts.jsonl", 8 * 1024 * 1024),
    RawArtifactContract("safety-summary", "safety-summary.json", 1024 * 1024),
    RawArtifactContract("serial-log", "serial.log", 16 * 1024 * 1024),
    RawArtifactContract("v1-peer-receipts", "v1-peer-receipts.jsonl", 8 * 1024 * 1024),
)
BSC09_RAW_ARTIFACTS = (
    RawArtifactContract("browser-projection", "browser-projection.json", 2 * 1024 * 1024),
    RawArtifactContract("case-observation", "case-observation.json", 2 * 1024 * 1024),
    RawArtifactContract("firmware-build", "firmware-build.json", 1024 * 1024),
    RawArtifactContract("health-projection", "health-projection.json", 1024 * 1024),
    RawArtifactContract("heap-projection", "heap-projection.json", 2 * 1024 * 1024),
    RawArtifactContract("wifi-mode-projection", "wifi-mode-projection.json", 1024 * 1024),
    RawArtifactContract("wifi-scan-projection", "wifi-scan-projection.json", 2 * 1024 * 1024),
)

RAW_ARTIFACTS_BY_CASE: Mapping[str, tuple[RawArtifactContract, ...]] = MappingProxyType(
    {
        "BSC-07": BSC07_RAW_ARTIFACTS,
        "BSC-08": BSC08_RAW_ARTIFACTS,
        "BSC-09": BSC09_RAW_ARTIFACTS,
        "BSC-12": BSC12_RAW_ARTIFACTS,
        "BSC-16": BSC16_RAW_ARTIFACTS,
    }
)


def _environment_for(build_kind: BuildKind) -> str:
    if build_kind == "hil-fault":
        return "waveshare-349-hil"
    if build_kind == "production":
        return "waveshare-349"
    if build_kind == "car-production":
        return "esp32-s3-car-install"
    raise RigAdapterContractError("rig-adapter build kind is invalid")


def _role(
    role_id: str,
    build_kind: BuildKind,
    raw_artifacts: tuple[RawArtifactContract, ...] = RAW_ARTIFACTS,
) -> AdapterRoleContract:
    return AdapterRoleContract(
        role_id=role_id,
        build_kind=build_kind,
        firmware_environment=_environment_for(build_kind),
        raw_artifacts=raw_artifacts,
    )


def _adapter(
    case_id: str,
    *,
    minimum_runs: int,
    dut: tuple[str, ...],
    rig: tuple[str, ...],
    roles: tuple[tuple[str, BuildKind], ...],
    raw_artifacts: tuple[RawArtifactContract, ...] = RAW_ARTIFACTS,
) -> RigAdapter:
    return RigAdapter(
        case_id=case_id,
        adapter_id=f"bug-squash-{case_id.lower()}-rig-v1",
        protocol_version=ADAPTER_PROTOCOL_VERSION,
        status="unavailable",
        source_path=None,
        entrypoint=None,
        minimum_runs=minimum_runs,
        required_dut_capabilities=dut,
        required_rig_capabilities=rig,
        roles=tuple(_role(role_id, build_kind, raw_artifacts) for role_id, build_kind in roles),
    )


_ADAPTERS = (
    _adapter(
        "BSC-02",
        minimum_runs=1,
        dut=("firmware-execution", "maintenance-mode", "serial"),
        rig=("artifact-capture", "lan-client", "sram-pressure-control", "utc-time-source"),
        roles=(
            ("maintenance-recovery-fault", "hil-fault"),
            ("maintenance-recovery-production-replay", "production"),
        ),
    ),
    _adapter(
        "BSC-03",
        minimum_runs=3,
        dut=("firmware-execution", "persistence", "serial"),
        rig=(
            "artifact-capture",
            "bond-peer",
            "obd-peer",
            "power-control",
            "sd-media",
            "utc-time-source",
            "v1-peer",
            "vbus-isolation",
        ),
        roles=(("connected-persistence-hard-cut", "production"),),
    ),
    _adapter(
        "BSC-04",
        minimum_runs=1,
        dut=("firmware-execution", "serial", "v1-connectivity"),
        rig=("artifact-capture", "obd-peer", "proxy-client", "utc-time-source", "v1-power-control"),
        roles=(
            ("late-v1-verify-timeout-fault", "hil-fault"),
            ("late-v1-production-replay", "production"),
        ),
    ),
    _adapter(
        "BSC-05",
        minimum_runs=3,
        dut=("display", "firmware-execution", "serial", "v1-connectivity"),
        rig=("artifact-capture", "display-capture", "programmable-v1-peer", "utc-time-source"),
        roles=(
            ("alert-generation-fence", "hil-fault"),
            ("alert-generation-production-replay", "production"),
        ),
    ),
    _adapter(
        "BSC-06",
        minimum_runs=3,
        dut=("firmware-execution", "obd-connectivity", "proxy-connectivity", "serial"),
        rig=("artifact-capture", "obd-peer", "proxy-client", "utc-time-source", "v1-peer"),
        roles=(
            ("obd-transport-race-fault", "hil-fault"),
            ("obd-transport-production-replay", "production"),
        ),
    ),
    _adapter(
        "BSC-07",
        minimum_runs=1,
        dut=("battery-monitor", "firmware-execution", "maintenance-mode", "power-button", "serial"),
        rig=("ap-traffic", "artifact-capture", "power-control", "utc-time-source", "vbus-isolation"),
        roles=(("maintenance-power-safety", "production"),),
        raw_artifacts=BSC07_RAW_ARTIFACTS,
    ),
    _adapter(
        "BSC-08",
        minimum_runs=3,
        dut=("firmware-execution", "proxy-connectivity", "serial", "v1-connectivity"),
        rig=("artifact-capture", "proxy-client", "utc-time-source", "v1-peer"),
        roles=(("proxy-epoch-teardown", "production"),),
        raw_artifacts=BSC08_RAW_ARTIFACTS,
    ),
    _adapter(
        "BSC-09",
        minimum_runs=3,
        dut=("firmware-execution", "maintenance-mode", "serial", "wifi-scan"),
        rig=("artifact-capture", "browser-client", "multiple-access-points", "utc-time-source"),
        roles=(("wifi-dual-consumer-scan", "production"),),
        raw_artifacts=BSC09_RAW_ARTIFACTS,
    ),
    _adapter(
        "BSC-10",
        minimum_runs=1,
        dut=("firmware-execution", "maintenance-mode", "serial", "wifi-client"),
        rig=("artifact-capture", "browser-client", "http-response-control", "utc-time-source"),
        roles=(
            ("wifi-enable-transaction-fault", "hil-fault"),
            ("wifi-enable-production-replay", "production"),
        ),
    ),
    _adapter(
        "BSC-11",
        minimum_runs=1,
        dut=("car-mode", "firmware-execution", "serial", "v1-connectivity"),
        rig=("artifact-capture", "ignition-control", "power-button", "utc-time-source", "vbus-isolation"),
        roles=(("car-shutdown-isolation", "car-production"),),
    ),
    _adapter(
        "BSC-12",
        minimum_runs=1,
        dut=("firmware-execution", "persistence", "power-button", "serial"),
        rig=(
            "artifact-capture",
            "bond-peer",
            "power-control",
            "sd-media",
            "utc-time-source",
            "vbus-isolation",
            "wake-input-control",
        ),
        roles=(("aborted-shutdown-recovery", "production"),),
        raw_artifacts=BSC12_RAW_ARTIFACTS,
    ),
    _adapter(
        "BSC-13",
        minimum_runs=3,
        dut=("firmware-execution", "obd-connectivity", "proxy-connectivity", "serial"),
        rig=("artifact-capture", "obd-peer", "proxy-client", "utc-time-source"),
        roles=(
            ("obd-connect-edge-preemption-fault", "hil-fault"),
            ("obd-connect-edge-production-replay", "production"),
        ),
    ),
    _adapter(
        "BSC-14",
        minimum_runs=1,
        dut=("firmware-execution", "persistence", "serial", "touchscreen"),
        rig=("artifact-capture", "reset-control", "sd-media", "utc-time-source"),
        roles=(
            ("touch-persistence-sd-fault", "hil-fault"),
            ("touch-persistence-production-replay", "production"),
        ),
    ),
    RigAdapter(
        case_id="BSC-16",
        adapter_id="bug-squash-bsc-16-rig-v1",
        protocol_version=ADAPTER_PROTOCOL_VERSION,
        status="implemented",
        source_path="scripts/bug_squash_hil_bsc16_rig.py",
        entrypoint="main",
        minimum_runs=1,
        required_dut_capabilities=(
            "battery-monitor",
            "firmware-execution",
            "power-button",
            "serial",
        ),
        required_rig_capabilities=(
            "artifact-capture",
            "battery-source",
            "logic-analyzer",
            "power-control",
            "usb-source",
            "utc-time-source",
            "vbus-isolation",
        ),
        roles=(
            _role("battery-source-policy-fault", "hil-fault", BSC16_RAW_ARTIFACTS),
            _role("battery-source-production-replay", "production", BSC16_RAW_ARTIFACTS),
        ),
    ),
)


def _validate_capabilities(values: tuple[str, ...], label: str) -> None:
    if not values or tuple(sorted(values)) != values or len(set(values)) != len(values):
        raise RigAdapterContractError(f"{label} capabilities must be sorted and unique")
    if any(SAFE_SLUG_RE.fullmatch(value) is None for value in values):
        raise RigAdapterContractError(f"{label} capability is unsafe")


def _validate_raw_artifacts(artifacts: tuple[RawArtifactContract, ...]) -> None:
    if not artifacts:
        raise RigAdapterContractError("raw-artifact contract is empty")
    roles = tuple(artifact.role for artifact in artifacts)
    filenames = tuple(artifact.filename for artifact in artifacts)
    if len(set(roles)) != len(roles) or len(set(filenames)) != len(filenames):
        raise RigAdapterContractError("raw-artifact roles and filenames must be unique")
    for artifact in artifacts:
        if SAFE_SLUG_RE.fullmatch(artifact.role) is None:
            raise RigAdapterContractError("raw-artifact role is unsafe")
        if (
            SAFE_FILENAME_RE.fullmatch(artifact.filename) is None
            or artifact.filename in {".", ".."}
            or "/" in artifact.filename
            or "\\" in artifact.filename
        ):
            raise RigAdapterContractError("raw-artifact filename is unsafe")
        if (
            type(artifact.maximum_bytes) is not int
            or not 1 <= artifact.maximum_bytes <= MAX_RAW_ARTIFACT_BYTES
        ):
            raise RigAdapterContractError("raw-artifact size limit is invalid")


def validate_adapter_descriptor(adapter: RigAdapter) -> None:
    if adapter.case_id not in CASE_IDS:
        raise RigAdapterContractError("rig-adapter case ID is unknown")
    if adapter.adapter_id != f"bug-squash-{adapter.case_id.lower()}-rig-v1":
        raise RigAdapterContractError("rig-adapter identity is invalid")
    if (
        type(adapter.protocol_version) is not int
        or adapter.protocol_version != ADAPTER_PROTOCOL_VERSION
    ):
        raise RigAdapterContractError("rig-adapter protocol version is invalid")
    if type(adapter.minimum_runs) is not int or not 1 <= adapter.minimum_runs <= 10:
        raise RigAdapterContractError("rig-adapter run count is invalid")
    _validate_capabilities(adapter.required_dut_capabilities, "DUT")
    _validate_capabilities(adapter.required_rig_capabilities, "rig")
    if not adapter.roles or len({role.role_id for role in adapter.roles}) != len(adapter.roles):
        raise RigAdapterContractError("rig-adapter roles must be nonempty and unique")
    for role in adapter.roles:
        if SAFE_SLUG_RE.fullmatch(role.role_id) is None:
            raise RigAdapterContractError("rig-adapter role identity is unsafe")
        if role.firmware_environment != _environment_for(role.build_kind):
            raise RigAdapterContractError("rig-adapter firmware environment is inconsistent")
        _validate_raw_artifacts(role.raw_artifacts)
        expected_artifacts = RAW_ARTIFACTS_BY_CASE.get(adapter.case_id, RAW_ARTIFACTS)
        if role.raw_artifacts != expected_artifacts:
            raise RigAdapterContractError("rig-adapter raw-artifact contract drifted")
    if adapter.status not in {"unavailable", "implemented"}:
        raise RigAdapterContractError("rig-adapter status is invalid")
    if adapter.implemented:
        if (
            not isinstance(adapter.source_path, str)
            or SAFE_SOURCE_RE.fullmatch(adapter.source_path) is None
            or not isinstance(adapter.entrypoint, str)
            or SAFE_ENTRYPOINT_RE.fullmatch(adapter.entrypoint) is None
        ):
            raise RigAdapterContractError("implemented rig adapter lacks a safe tracked entrypoint")
    elif adapter.source_path is not None or adapter.entrypoint is not None:
        raise RigAdapterContractError("unavailable rig adapter cannot name executable code")


def _validate_registry(adapters: tuple[RigAdapter, ...]) -> None:
    if tuple(adapter.case_id for adapter in adapters) != CASE_IDS:
        raise RigAdapterContractError("rig-adapter registry does not match the pinned case set")
    if len({adapter.adapter_id for adapter in adapters}) != len(adapters):
        raise RigAdapterContractError("rig-adapter registry contains duplicate identities")
    for adapter in adapters:
        validate_adapter_descriptor(adapter)


_validate_registry(_ADAPTERS)
ADAPTERS: tuple[RigAdapter, ...] = _ADAPTERS
ADAPTER_BY_CASE: Mapping[str, RigAdapter] = MappingProxyType(
    {adapter.case_id: adapter for adapter in ADAPTERS}
)


def get_rig_adapter(case_id: str) -> RigAdapter:
    """Resolve exactly one tracked adapter descriptor for a pinned case."""

    try:
        return ADAPTER_BY_CASE[case_id]
    except KeyError as exc:
        raise RigAdapterContractError("case ID is not present in the rig-adapter registry") from exc
