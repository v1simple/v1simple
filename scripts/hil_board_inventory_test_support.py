#!/usr/bin/env python3
"""Ephemeral SSHSIG fixtures for board-inventory regression tests."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess

import resolve_hil_board as resolver


def create_test_signer(directory: Path) -> tuple[Path, Path]:
    directory.mkdir(parents=True, exist_ok=True)
    signing_key = directory / "board-inventory-test-key"
    subprocess.run(
        [
            str(resolver.SSH_KEYGEN),
            "-q",
            "-t",
            "ed25519",
            "-N",
            "",
            "-C",
            "synthetic board inventory test signer",
            "-f",
            str(signing_key),
        ],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    fields = Path(f"{signing_key}.pub").read_text(encoding="ascii").split()
    if len(fields) < 2 or fields[0] != "ssh-ed25519":
        raise RuntimeError("synthetic board inventory test key is invalid")
    trust_root = directory / "board-inventory-test-allowed-signers"
    trust_root.write_text(
        (
            f'{resolver.INVENTORY_SIGNER_PRINCIPAL} '
            f'namespaces="{resolver.INVENTORY_SIGNATURE_NAMESPACE}" '
            f"{fields[0]} {fields[1]}\n"
        ),
        encoding="ascii",
    )
    return signing_key, trust_root


def sign_inventory(inventory_path: Path, signing_key: Path) -> Path:
    signature_path = Path(f"{inventory_path}.sig")
    signature_path.unlink(missing_ok=True)
    subprocess.run(
        [
            str(resolver.SSH_KEYGEN),
            "-q",
            "-Y",
            "sign",
            "-f",
            str(signing_key),
            "-n",
            resolver.INVENTORY_SIGNATURE_NAMESPACE,
            str(inventory_path),
        ],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return signature_path


def test_environment(trust_root: Path) -> dict[str, str]:
    environment = os.environ.copy()
    environment.update(
        {
            "V1SIMPLE_HIL_TEST_HOOKS": "1",
            "V1SIMPLE_HIL_TEST_BOARD_TRUST_ROOT": str(trust_root),
        }
    )
    return environment
