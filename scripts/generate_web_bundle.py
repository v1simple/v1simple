#!/usr/bin/env python3
"""Build, validate and embed the current web UI without reading device data."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import zlib


ROOT = Path(__file__).resolve().parents[1]
ROUTES = frozenset(("alp", "audio", "autopush", "colors", "devices", "gps",
                    "index", "obd", "profiles", "settings"))
STAMP = ".artifacts/web-bundle.json"
SCHEMA = 1
EXCLUDED_INPUT_DIRS = frozenset(("node_modules", "build", ".svelte-kit", ".vite"))
SAFE_PATH = re.compile(r"(?:[A-Za-z0-9_-][A-Za-z0-9_.-]*/)*[A-Za-z0-9_-][A-Za-z0-9_.-]*\Z")
APP_ASSET = re.compile(r"_app/(?:env\.js|version\.json|immutable/(?:assets|chunks|entry|nodes)/"
                       r"[A-Za-z0-9_-][A-Za-z0-9_.-]*\.(?:js|css|json|woff2?|ttf|png|svg|ico))(?:\.gz)?\Z")


def digest(files: dict[str, bytes]) -> str:
    """Length framing makes names and arbitrary file bytes unambiguous."""
    value = hashlib.sha256()
    for name, data in sorted(files.items()):
        encoded = name.encode("utf-8")
        value.update(len(encoded).to_bytes(8, "big"))
        value.update(encoded)
        value.update(len(data).to_bytes(8, "big"))
        value.update(data)
    return value.hexdigest()


def tree(directory: Path, excluded: frozenset[str] = frozenset()) -> dict[str, bytes]:
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError(f"missing or unsafe directory: {directory.name}")
    files: dict[str, bytes] = {}

    def visit(parent: Path) -> None:
        for item in sorted(parent.iterdir()):
            if (parent == directory and item.name in excluded) or item.name == ".DS_Store":
                continue
            if item.is_symlink():
                raise ValueError("symlinks are not allowed in web inputs or outputs")
            if item.is_dir():
                visit(item)
            elif item.is_file():
                files[item.relative_to(directory).as_posix()] = item.read_bytes()
            else:
                raise ValueError("unsupported web input or output entry")

    visit(directory)
    return files


def inputs(root: Path) -> dict[str, bytes]:
    files = {"interface/" + name: data
             for name, data in tree(root / "interface", EXCLUDED_INPUT_DIRS).items()}
    for name in ("tools/compress_web_assets.sh", "scripts/generate_web_bundle.py",
                 "scripts/embed_web_assets.py"):
        path = root / name
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"missing or unsafe build input: {name}")
        files[name] = path.read_bytes()
    for name in ("interface/package.json", "interface/package-lock.json", "interface/vite.config.js"):
        if name not in files:
            raise ValueError(f"missing required build input: {name}")
    return files


def select_assets(outputs: dict[str, bytes]) -> dict[str, bytes]:
    """Match deployment pruning; never select audio, settings or user files."""
    selected: dict[str, bytes] = {}
    for name, data in sorted(outputs.items()):
        managed = (name in {route + ".html" for route in ROUTES} or
                   name.startswith("_app/") or
                   (name.startswith("branding/") and "/" not in name[len("branding/"):] and
                    name.endswith(".png")))
        if not managed:
            continue
        if not SAFE_PATH.fullmatch(name) or any(part in (".", "..") for part in name.split("/")):
            raise ValueError("unsafe generated asset path")
        if name.startswith("_app/") and not APP_ASSET.fullmatch(name):
            raise ValueError("unexpected file in generated app assets")
        if not data:
            raise ValueError(f"empty generated asset: {name}")
        if name.endswith(".gz"):
            raw_name = name[:-3]
            if raw_name not in outputs or not raw_name.endswith((".js", ".css", ".json")):
                raise ValueError(f"compressed asset has no supported original: {name}")
            try:
                uncompressed = gzip.decompress(data)
            except (OSError, EOFError, zlib.error) as error:
                raise ValueError(f"invalid compressed asset: {name}") from error
            if uncompressed != outputs[raw_name]:
                raise ValueError(f"compressed asset differs from its original: {name}")
        elif name.startswith("_app/") and name.endswith((".js", ".css", ".json")):
            if name + ".gz" not in outputs:
                raise ValueError(f"missing compressed asset: {name}")
            if name not in ("_app/env.js", "_app/version.json"):
                continue
        selected["/" + name] = data
    for route in sorted(ROUTES):
        if "/" + route + ".html" not in selected:
            raise ValueError(f"missing generated route: {route}")
    if "/_app/version.json" not in selected:
        raise ValueError("missing generated app version")
    if not any(name.startswith("/_app/immutable/entry/") and name.endswith(".js.gz")
               for name in selected):
        raise ValueError("missing generated app entry")
    return selected


def snapshot(root: Path) -> tuple[dict, dict[str, bytes]]:
    source_files = inputs(root)
    source_digest = digest(source_files)
    outputs = tree(root / "interface/build")
    assets = select_assets(outputs)
    for name, data in source_files.items():
        if name.startswith("interface/static/branding/") and name.endswith(".png"):
            deployed_name = "/" + name.removeprefix("interface/static/")
            if assets.get(deployed_name) != data:
                raise ValueError("built branding differs from current static assets")
    record = {"schema": SCHEMA, "inputs_sha256": source_digest,
              "outputs_sha256": digest(outputs), "bundle_sha256": digest(assets),
              "asset_count": len(assets), "asset_bytes": sum(map(len, assets.values()))}
    return record, assets


def write_changed(path: Path, data: bytes) -> bool:
    if any(parent.is_symlink() for parent in (path, *path.parents)):
        raise ValueError("refusing symlink output")
    if path.exists() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    if temporary.is_symlink():
        raise ValueError("refusing symlink temporary output")
    temporary.write_bytes(data)
    temporary.replace(path)
    return True


def build(root: Path) -> dict:
    source_digest = digest(inputs(root))
    stamp = root / STAMP
    if stamp.is_symlink() or stamp.parent.is_symlink():
        raise ValueError("refusing symlink build stamp")
    stamp.unlink(missing_ok=True)
    subprocess.run(["vite", "build"], cwd=root / "interface", check=True)
    subprocess.run(["bash", str(root / "tools/compress_web_assets.sh")], cwd=root, check=True)
    record, _ = snapshot(root)
    if record["inputs_sha256"] != source_digest:
        raise ValueError("frontend inputs changed during build; run npm run build again")
    write_changed(stamp, (json.dumps(record, sort_keys=True, indent=2) + "\n").encode())
    return record


def validate(root: Path) -> tuple[dict, dict[str, bytes]]:
    stamp = root / STAMP
    if stamp.is_symlink() or stamp.parent.is_symlink() or not stamp.is_file():
        raise ValueError("web build stamp missing; run npm run build in interface")
    try:
        expected = json.loads(stamp.read_text())
    except (OSError, ValueError) as error:
        raise ValueError("invalid web build stamp; run npm run build in interface") from error
    actual, assets = snapshot(root)
    if expected != actual:
        raise ValueError("web build is stale or modified; run npm run build in interface")
    return actual, assets


def fingerprint(record: dict) -> str:
    return "V1SimpleWebBundle:" + record["bundle_sha256"]


def generate(root: Path, output: Path) -> dict:
    record, assets = validate(root)
    lines = ['// Generated from a verified frontend build; do not edit.', '#pragma once',
             '#include "modules/wifi/embedded_web_assets.h"', '', 'namespace EmbeddedWebAssets {',
             f'static const char kBundleFingerprint[] = "{fingerprint(record)}";']
    for index, data in enumerate(assets.values()):
        lines.append(f'static const uint8_t kAssetData{index}[] = {{')
        for offset in range(0, len(data), 20):
            lines.append('    ' + ','.join(f'0x{byte:02x}' for byte in data[offset:offset + 20]) + ',')
        lines.append('};')
    lines.append('static const Asset kAssets[] = {')
    for index, name in enumerate(assets):
        lines.append(f'    {{"{name}", kAssetData{index}, sizeof(kAssetData{index})}},')
    lines.extend(('};', '} // namespace EmbeddedWebAssets', ''))
    write_changed(output, '\n'.join(lines).encode())
    return record


def verify_firmware(root: Path, firmware: Path) -> dict:
    record, assets = validate(root)
    if firmware.is_symlink() or not firmware.is_file():
        raise ValueError("missing or unsafe firmware image")
    image = firmware.read_bytes()
    if (fingerprint(record).encode() + b'\0') not in image:
        raise ValueError("firmware does not contain the current web bundle fingerprint")
    for name, data in assets.items():
        if name.encode() + b'\0' not in image or data not in image:
            raise ValueError(f"firmware is missing the current embedded asset: {name}")
    return record


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    subparsers = parser.add_subparsers(dest='command', required=True)
    subparsers.add_parser('build')
    subparsers.add_parser('validate')
    generator = subparsers.add_parser('generate')
    generator.add_argument('--output', type=Path, required=True)
    verifier = subparsers.add_parser('verify-firmware')
    verifier.add_argument('firmware', type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == 'build':
            record = build(args.root)
        elif args.command == 'generate':
            record = generate(args.root, args.output)
        elif args.command == 'verify-firmware':
            record = verify_firmware(args.root, args.firmware)
        else:
            record, _ = validate(args.root)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'[web-bundle] {error}', file=sys.stderr)
        return 1
    print(f'[web-bundle] {args.command}: {record["asset_count"]} assets, '
          f'{record["asset_bytes"]} bytes, {record["bundle_sha256"]}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
