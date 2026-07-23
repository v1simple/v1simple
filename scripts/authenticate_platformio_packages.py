#!/usr/bin/env python3
"""Authenticate the PlatformIO build package graph against tracked vendor roots."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import platform
import re
import stat
import subprocess
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
TRUST_ROOT = ROOT / "tools" / "bug_squash_platformio_build_root_v1.json"
TREE_DOMAIN = b"v1simple.platformio-package-tree.v1\0"
IDENTITY_DOMAIN = b"v1simple.platformio-package-set.v1\0"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
GIT_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
SAFE_RELATIVE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._ +\-/]{0,255}$")


class AuthenticationError(ValueError):
    """Raised when an observed build input cannot be authenticated."""


def _duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise AuthenticationError(f"duplicate trust-root key: {key}")
        result[key] = value
    return result


def _sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def _canonical_bytes(payload: Any) -> bytes:
    return json.dumps(
        payload,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("ascii")


def _exact_keys(value: object, expected: set[str], label: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != expected:
        raise AuthenticationError(f"{label} has an invalid schema")
    return value


def _full_sha256(value: object, label: str) -> str:
    if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
        raise AuthenticationError(f"{label} is not a full SHA-256")
    return value


def _full_git_sha(value: object, label: str) -> str:
    if not isinstance(value, str) or GIT_SHA_RE.fullmatch(value) is None:
        raise AuthenticationError(f"{label} is not a full Git identity")
    return value


def _https_url(value: object, label: str) -> str:
    if not isinstance(value, str) or not value.startswith("https://"):
        raise AuthenticationError(f"{label} is not an HTTPS vendor root")
    return value


def _validate_source(value: object, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise AuthenticationError(f"{label} must be an object")
    authentication = value.get("authentication")
    if authentication == "sha256":
        allowed = {"authentication", "url", "sha256", "payload_sha256"}
        if not set(value).issubset(allowed) or not {
            "authentication",
            "url",
            "sha256",
        }.issubset(value):
            raise AuthenticationError(f"{label} SHA-256 root has an invalid schema")
        _https_url(value["url"], f"{label}.url")
        _full_sha256(value["sha256"], f"{label}.sha256")
        payloads = value.get("payload_sha256", {})
        if not isinstance(payloads, dict):
            raise AuthenticationError(f"{label}.payload_sha256 must be an object")
        for key, digest in payloads.items():
            if not isinstance(key, str) or not key:
                raise AuthenticationError(f"{label} has an invalid payload platform")
            _full_sha256(digest, f"{label}.payload_sha256.{key}")
        return value
    if authentication in {"signed-git", "git-commit-lock"}:
        expected = {"authentication", "repository", "commit", "tree"}
        if authentication == "signed-git":
            expected.add("signature")
        source = _exact_keys(value, expected, label)
        _https_url(source["repository"], f"{label}.repository")
        _full_git_sha(source["commit"], f"{label}.commit")
        _full_git_sha(source["tree"], f"{label}.tree")
        if authentication == "signed-git" and source["signature"] != "github-verified-commit":
            raise AuthenticationError(f"{label} signature identity is invalid")
        return source
    raise AuthenticationError(f"{label} has an unsupported authentication root")


def _validate_tree_pins(value: object, label: str) -> dict[str, list[str]]:
    if not isinstance(value, dict) or not value:
        raise AuthenticationError(f"{label} must contain tree pins")
    result: dict[str, list[str]] = {}
    for platform_key, raw_digests in value.items():
        if not isinstance(platform_key, str) or not platform_key:
            raise AuthenticationError(f"{label} has an invalid platform key")
        if not isinstance(raw_digests, list) or not raw_digests:
            raise AuthenticationError(f"{label}.{platform_key} must contain pins")
        digests = [
            _full_sha256(digest, f"{label}.{platform_key}")
            for digest in raw_digests
        ]
        if len(set(digests)) != len(digests):
            raise AuthenticationError(f"{label}.{platform_key} contains duplicate pins")
        result[platform_key] = digests
    return result


def load_trust_root(path: Path = TRUST_ROOT) -> dict[str, Any]:
    try:
        metadata = os.lstat(path)
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
            raise AuthenticationError("PlatformIO trust root must be a regular file")
        content = path.read_bytes()
        payload = json.loads(content, object_pairs_hook=_duplicate_keys)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise AuthenticationError(
            f"PlatformIO trust root is unavailable: {type(exc).__name__}"
        ) from exc
    root = _exact_keys(
        payload,
        {
            "schema_version",
            "qualification_environments",
            "platformio_core",
            "packages",
        },
        "PlatformIO trust root",
    )
    if root["schema_version"] != 1:
        raise AuthenticationError("PlatformIO trust root schema is unsupported")
    environments = root["qualification_environments"]
    if (
        not isinstance(environments, list)
        or not environments
        or len(set(environments)) != len(environments)
        or any(not isinstance(item, str) or not item for item in environments)
    ):
        raise AuthenticationError("PlatformIO qualification environments are invalid")

    core = _exact_keys(
        root["platformio_core"],
        {"version", "launcher_body_sha256", "sources"},
        "PlatformIO core root",
    )
    if core["version"] != "6.1.19":
        raise AuthenticationError("PlatformIO core version root is invalid")
    _full_sha256(
        core["launcher_body_sha256"],
        "PlatformIO core launcher_body_sha256",
    )
    sources = core["sources"]
    if not isinstance(sources, list) or not sources:
        raise AuthenticationError("PlatformIO core has no authenticated sources")
    source_ids: set[str] = set()
    for index, raw_source in enumerate(sources):
        source = _exact_keys(
            raw_source,
            {
                "id",
                "source",
                "module_tree_sha256",
                "python_package_sha256",
            },
            f"PlatformIO core source {index}",
        )
        source_id = source["id"]
        if not isinstance(source_id, str) or not source_id or source_id in source_ids:
            raise AuthenticationError("PlatformIO core source identity is invalid")
        source_ids.add(source_id)
        _validate_source(source["source"], f"PlatformIO core source {source_id}")
        _full_sha256(
            source["module_tree_sha256"],
            f"PlatformIO core source {source_id}.module_tree_sha256",
        )
        _full_sha256(
            source["python_package_sha256"],
            f"PlatformIO core source {source_id}.python_package_sha256",
        )

    packages = root["packages"]
    if not isinstance(packages, list) or not packages:
        raise AuthenticationError("PlatformIO package root is empty")
    package_names: set[str] = set()
    for index, raw_package in enumerate(packages):
        allowed = {
            "name",
            "kind",
            "version",
            "location",
            "metadata",
            "source",
            "tree_sha256",
            "python_package_sha256",
        }
        if not isinstance(raw_package, dict) or not set(raw_package).issubset(allowed):
            raise AuthenticationError(f"PlatformIO package {index} has an invalid schema")
        if not {
            "name",
            "kind",
            "version",
            "location",
            "metadata",
            "source",
            "tree_sha256",
        }.issubset(raw_package):
            raise AuthenticationError(f"PlatformIO package {index} is incomplete")
        name = raw_package["name"]
        if not isinstance(name, str) or not name or name in package_names:
            raise AuthenticationError("PlatformIO package identity is invalid")
        package_names.add(name)
        if raw_package["kind"] not in {
            "compiler",
            "framework",
            "library",
            "platform",
            "tool",
        }:
            raise AuthenticationError(f"PlatformIO package {name} has an invalid kind")
        if not isinstance(raw_package["version"], str) or not raw_package["version"]:
            raise AuthenticationError(f"PlatformIO package {name} has an invalid version")
        location = _exact_keys(
            raw_package["location"],
            {"scope", "path"},
            f"PlatformIO package {name}.location",
        )
        if location["scope"] not in {"platformio-home", "project-libdeps"}:
            raise AuthenticationError(f"PlatformIO package {name} has an invalid scope")
        if (
            not isinstance(location["path"], str)
            or SAFE_RELATIVE_RE.fullmatch(location["path"]) is None
            or ".." in Path(location["path"]).parts
        ):
            raise AuthenticationError(f"PlatformIO package {name} has an unsafe path")
        metadata_root = raw_package["metadata"]
        if not isinstance(metadata_root, dict) or metadata_root.get("type") not in {
            "git",
            "library",
            "platform",
            "tool",
        }:
            raise AuthenticationError(f"PlatformIO package {name} metadata is invalid")
        if metadata_root["type"] == "git":
            _exact_keys(
                metadata_root,
                {"type", "commit"},
                f"PlatformIO package {name}.metadata",
            )
            _full_git_sha(
                metadata_root["commit"],
                f"PlatformIO package {name}.metadata.commit",
            )
        else:
            _exact_keys(
                metadata_root,
                {"type", "name", "version"},
                f"PlatformIO package {name}.metadata",
            )
            if (
                not isinstance(metadata_root["name"], str)
                or not metadata_root["name"]
                or metadata_root["version"] != raw_package["version"]
            ):
                raise AuthenticationError(
                    f"PlatformIO package {name} metadata identity is invalid"
                )
        _validate_source(raw_package["source"], f"PlatformIO package {name}.source")
        _validate_tree_pins(
            raw_package["tree_sha256"],
            f"PlatformIO package {name}.tree_sha256",
        )
        if "python_package_sha256" in raw_package:
            _full_sha256(
                raw_package["python_package_sha256"],
                f"PlatformIO package {name}.python_package_sha256",
            )
    return root


def host_platform_key() -> str:
    system = platform.system().lower()
    machine = platform.machine().lower()
    if system == "darwin" and machine in {"arm64", "aarch64"}:
        return "macos-arm64"
    if system == "darwin" and machine in {"amd64", "x86_64"}:
        return "macos-amd64"
    if system == "linux" and machine in {"amd64", "x86_64"}:
        return "linux-amd64"
    if system == "linux" and machine in {"arm64", "aarch64"}:
        return "linux-arm64"
    raise AuthenticationError("host platform has no authenticated package root")


def platformio_module_tree_pins(path: Path = TRUST_ROOT) -> set[str]:
    root = load_trust_root(path)
    return {
        source["module_tree_sha256"]
        for source in root["platformio_core"]["sources"]
    }


def authenticate_platformio_core(
    *,
    module_tree_sha256: str | None = None,
    python_package_sha256: str | None = None,
    trust_root_path: Path = TRUST_ROOT,
) -> dict[str, Any]:
    if module_tree_sha256 is None and python_package_sha256 is None:
        raise AuthenticationError("PlatformIO core observation is missing")
    root = load_trust_root(trust_root_path)
    for source in root["platformio_core"]["sources"]:
        if (
            module_tree_sha256 is not None
            and source["module_tree_sha256"] != module_tree_sha256
        ):
            continue
        if (
            python_package_sha256 is not None
            and source["python_package_sha256"] != python_package_sha256
        ):
            continue
        return {
            "id": source["id"],
            "source": source["source"],
            "version": root["platformio_core"]["version"],
        }
    raise AuthenticationError("observed PlatformIO core identity is not authenticated")


def authenticate_platformio_launcher(
    launcher: Path,
    python: Path,
    *,
    trust_root_path: Path = TRUST_ROOT,
) -> None:
    try:
        launcher_stat = os.lstat(launcher)
        python_stat = os.lstat(python)
        if (
            stat.S_ISLNK(launcher_stat.st_mode)
            or not stat.S_ISREG(launcher_stat.st_mode)
            or stat.S_ISLNK(python_stat.st_mode)
            or not stat.S_ISREG(python_stat.st_mode)
        ):
            raise AuthenticationError("PlatformIO launcher identity is unsafe")
        lines = launcher.read_bytes().splitlines(keepends=True)
        shebang_python = Path(os.fsdecode(lines[0][2:].strip())) if lines else Path()
        if (
            len(lines) < 2
            or not lines[0].startswith(b"#!")
            or not shebang_python.is_absolute()
            or shebang_python.parent.resolve() != launcher.parent.resolve()
            or shebang_python.resolve() != python.resolve()
        ):
            raise AuthenticationError("PlatformIO launcher interpreter is substituted")
    except OSError as exc:
        raise AuthenticationError("PlatformIO launcher identity is unavailable") from exc
    root = load_trust_root(trust_root_path)
    if _sha256_bytes(b"".join(lines[1:])) != root["platformio_core"][
        "launcher_body_sha256"
    ]:
        raise AuthenticationError("PlatformIO launcher body is not authenticated")


def package_tree_sha256(root: Path) -> str:
    try:
        metadata = os.lstat(root)
    except OSError as exc:
        raise AuthenticationError(
            f"PlatformIO package tree is unavailable: {type(exc).__name__}"
        ) from exc
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        raise AuthenticationError("PlatformIO package tree is not a real directory")

    digest = hashlib.sha256(TREE_DOMAIN)
    files = 0
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if (
            any(
                part in {".git", "__pycache__"} or part.endswith(".egg-info")
                for part in relative.parts
            )
            or path.name in {".DS_Store", ".piopm"}
            or path.suffix in {".pyc", ".pyo"}
        ):
            continue
        relative_bytes = relative.as_posix().encode("utf-8")
        try:
            path_metadata = os.lstat(path)
            if stat.S_ISLNK(path_metadata.st_mode):
                kind = b"L"
                content = os.readlink(path).encode("utf-8")
                digest.update(
                    kind
                    + len(relative_bytes).to_bytes(4, "big")
                    + relative_bytes
                    + len(content).to_bytes(8, "big")
                    + content
                )
                files += 1
            elif stat.S_ISREG(path_metadata.st_mode):
                kind = b"F"
                digest.update(
                    kind
                    + len(relative_bytes).to_bytes(4, "big")
                    + relative_bytes
                    + path_metadata.st_size.to_bytes(8, "big")
                )
                with path.open("rb") as handle:
                    for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                        digest.update(chunk)
                files += 1
            elif not stat.S_ISDIR(path_metadata.st_mode):
                raise AuthenticationError(
                    f"PlatformIO package contains a special file: {relative.as_posix()}"
                )
        except OSError as exc:
            raise AuthenticationError(
                f"PlatformIO package tree changed while hashing: {type(exc).__name__}"
            ) from exc
    if files == 0:
        raise AuthenticationError("PlatformIO package tree is empty")
    return digest.hexdigest()


def _authenticate_metadata(package: dict[str, Any], package_root: Path) -> None:
    expected = package["metadata"]
    if expected["type"] == "git":
        result = subprocess.run(
            [
                "/usr/bin/git",
                "-C",
                str(package_root),
                "-c",
                "core.fsmonitor=false",
                "rev-parse",
                "--verify",
                "HEAD^{commit}",
            ],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
        if result.returncode != 0 or result.stdout.strip() != expected["commit"]:
            raise AuthenticationError(
                f"PlatformIO package {package['name']} Git identity is not pinned"
            )
        return

    metadata_path = package_root / ".piopm"
    try:
        metadata_stat = os.lstat(metadata_path)
        if stat.S_ISLNK(metadata_stat.st_mode) or not stat.S_ISREG(metadata_stat.st_mode):
            raise AuthenticationError(
                f"PlatformIO package {package['name']} metadata is unsafe"
            )
        observed = json.loads(
            metadata_path.read_text(encoding="utf-8"),
            object_pairs_hook=_duplicate_keys,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise AuthenticationError(
            f"PlatformIO package {package['name']} metadata is unavailable"
        ) from exc
    if (
        not isinstance(observed, dict)
        or observed.get("type") != expected["type"]
        or observed.get("name") != expected["name"]
        or observed.get("version") != expected["version"]
    ):
        raise AuthenticationError(
            f"PlatformIO package {package['name']} metadata identity is unknown"
        )


def _pins_for_platform(package: dict[str, Any], platform_key: str) -> list[str]:
    pins = package["tree_sha256"]
    selected = pins.get("any")
    if selected is None:
        selected = pins.get(platform_key)
    if not selected:
        raise AuthenticationError(
            f"PlatformIO package {package['name']} has no {platform_key} tree pin"
        )
    return selected


def authenticate_python_package(
    package_name: str,
    digest: str,
    *,
    trust_root_path: Path = TRUST_ROOT,
) -> None:
    root = load_trust_root(trust_root_path)
    for package in root["packages"]:
        if package["name"] != package_name:
            continue
        expected = package.get("python_package_sha256")
        if expected != digest:
            raise AuthenticationError(
                f"PlatformIO package {package_name} Python identity is not pinned"
            )
        return
    raise AuthenticationError(f"PlatformIO package {package_name} is unknown")


def authenticate_platformio_packages(
    project_root: Path = ROOT,
    *,
    platformio_home: Path | None = None,
    trust_root_path: Path | None = None,
    platform_key: str | None = None,
) -> dict[str, Any]:
    trust_path = (
        project_root / "tools" / TRUST_ROOT.name
        if trust_root_path is None
        else trust_root_path
    )
    trust = load_trust_root(trust_path)
    host_key = host_platform_key() if platform_key is None else platform_key
    pio_home = Path.home() / ".platformio" if platformio_home is None else platformio_home
    environments = tuple(trust["qualification_environments"])
    project_packages = [
        package
        for package in trust["packages"]
        if package["location"]["scope"] == "project-libdeps"
    ]
    expected_library_paths = {package["location"]["path"] for package in project_packages}

    for environment in environments:
        libdeps = project_root / ".pio" / "libdeps" / environment
        try:
            actual = {
                entry.name
                for entry in libdeps.iterdir()
                if entry.name != "integrity.dat"
            }
        except OSError as exc:
            raise AuthenticationError(
                f"PlatformIO library inventory is unavailable: {environment}"
            ) from exc
        if actual != expected_library_paths:
            raise AuthenticationError(
                f"PlatformIO library inventory is unknown: {environment}"
            )

    identity_packages: list[dict[str, Any]] = []
    for package in trust["packages"]:
        location = package["location"]
        if location["scope"] == "platformio-home":
            observed_roots = [pio_home / location["path"]]
        else:
            observed_roots = [
                project_root / ".pio" / "libdeps" / environment / location["path"]
                for environment in environments
            ]
        allowed_pins = _pins_for_platform(package, host_key)
        for observed_root in observed_roots:
            _authenticate_metadata(package, observed_root)
            observed_tree = package_tree_sha256(observed_root)
            if observed_tree not in allowed_pins:
                raise AuthenticationError(
                    f"PlatformIO package {package['name']} tree identity is not pinned"
                )
        identity_packages.append(
            {
                "kind": package["kind"],
                "name": package["name"],
                "source": package["source"],
                "tree_sha256": allowed_pins,
                "version": package["version"],
            }
        )

    identity: dict[str, Any] = {
        "schema_version": 1,
        "platform": host_key,
        "qualification_environments": list(environments),
        "trust_root_sha256": _sha256_bytes(trust_path.read_bytes()),
        "packages": identity_packages,
    }
    identity["identity_sha256"] = _sha256_bytes(
        IDENTITY_DOMAIN + _canonical_bytes(identity)
    )
    return identity
