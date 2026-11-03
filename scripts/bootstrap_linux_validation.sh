#!/bin/bash
# Install the declared Linux validation dependencies for GitHub CI or Release.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PROFILE="${1:-}"
PYTHON_CMD="${PYTHON_CMD:-python3}"
VALIDATION_PYTHON="$PYTHON_CMD"
HOST_TOOLS="$ROOT_DIR/scripts/host_tools.py"
host_tool() { "$PYTHON_CMD" "$HOST_TOOLS" "$@"; }

usage() {
  echo "Usage: scripts/bootstrap_linux_validation.sh <ci|release>" >&2
}

if [[ "$PROFILE" != "ci" && "$PROFILE" != "release" ]]; then
  usage
  exit 2
fi
# This uses the available base interpreter, before any desired tool is installed.
NODE_VERSION="$(host_tool get node)"
PYTHON_VERSION="$(host_tool get python)"
SHELLCHECK_VERSION="$(host_tool get shellcheck)"
SWIFT_VERSION="$(host_tool get swift)"
SWIFT_SHA256="$(host_tool get swift_sha256)"
LINUX_ID="$(host_tool get linux_id)"
LINUX_VERSION="$(host_tool get linux_version)"
package_lines="$(host_tool packages "$PROFILE")"
PYTHON_PACKAGES=()
while IFS= read -r package; do
  [[ -z "$package" ]] || PYTHON_PACKAGES+=("$package")
done <<< "$package_lines"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "[linux-bootstrap] Linux is required." >&2
  exit 1
fi
if [[ ! -r /etc/os-release ]]; then
  echo "[linux-bootstrap] /etc/os-release is unavailable." >&2
  exit 1
fi
# shellcheck disable=SC1091
source /etc/os-release
if [[ "${ID:-}" != "$LINUX_ID" || "${VERSION_ID:-}" != "$LINUX_VERSION" ]]; then
  echo "[linux-bootstrap] $LINUX_ID $LINUX_VERSION is required; found ${ID:-unknown} ${VERSION_ID:-unknown}." >&2
  exit 1
fi
if [[ "$("$PYTHON_CMD" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')" != "$PYTHON_VERSION" ]]; then
  echo "[linux-bootstrap] Python $PYTHON_VERSION is required." >&2
  exit 1
fi
if [[ "$(node --version 2>/dev/null || true)" != "v$NODE_VERSION" ]]; then
  echo "[linux-bootstrap] Node.js $NODE_VERSION is required." >&2
  exit 1
fi

APT=(apt-get)
if [[ "$EUID" -ne 0 ]]; then
  if ! command -v sudo >/dev/null 2>&1; then
    echo "[linux-bootstrap] root or sudo is required to install system dependencies." >&2
    exit 1
  fi
  APT=(sudo apt-get)
fi

SYSTEM_PACKAGES=()
for package in ffmpeg libpcre3; do
  if host_tool requires "$PROFILE" "$package"; then
    SYSTEM_PACKAGES+=("$package")
  fi
done
if ((${#SYSTEM_PACKAGES[@]} > 0)) || host_tool requires "$PROFILE" swift || host_tool requires "$PROFILE" shellcheck; then
  "${APT[@]}" update
fi
if ((${#SYSTEM_PACKAGES[@]} > 0)); then
  "${APT[@]}" install --no-install-recommends --yes "${SYSTEM_PACKAGES[@]}"
fi

if host_tool requires "$PROFILE" swift; then
  installed_swift_version="$(swiftc --version 2>/dev/null | sed -n 's/^Swift version \([^ ]*\).*/\1/p' | head -n 1 || true)"
  if [[ "$installed_swift_version" != "$SWIFT_VERSION" ]]; then
    "${APT[@]}" install --no-install-recommends --yes \
      binutils libc6-dev libcurl4-openssl-dev libedit2 libgcc-13-dev \
      libncurses-dev libpython3-dev libsqlite3-0 libstdc++-13-dev \
      libxml2-dev libz3-dev pkg-config unzip zip zlib1g-dev
    SWIFT_ROOT="${V1_SWIFT_ROOT:-/opt/swift-$SWIFT_VERSION}"
    swift_archive="/tmp/swift-${SWIFT_VERSION}-RELEASE-ubuntu${LINUX_VERSION}.tar.gz"
    curl --fail --location --silent --show-error \
      "https://download.swift.org/swift-${SWIFT_VERSION}-release/ubuntu${LINUX_VERSION//./}/swift-${SWIFT_VERSION}-RELEASE/swift-${SWIFT_VERSION}-RELEASE-ubuntu${LINUX_VERSION}.tar.gz" \
      --output "$swift_archive"
    printf '%s  %s\n' "$SWIFT_SHA256" "$swift_archive" | sha256sum --check --strict
    mkdir -p "$SWIFT_ROOT"
    tar -xzf "$swift_archive" -C "$SWIFT_ROOT" --strip-components=1
    export PATH="$SWIFT_ROOT/usr/bin:$PATH"
    if [[ -n "${GITHUB_PATH:-}" ]]; then
      printf '%s\n' "$SWIFT_ROOT/usr/bin" >> "$GITHUB_PATH"
    fi
  fi
fi

if host_tool requires "$PROFILE" shellcheck; then
  shellcheck_archive="/tmp/shellcheck-v${SHELLCHECK_VERSION}.linux.x86_64.tar.xz"
  curl --fail --location --silent --show-error \
    "https://github.com/koalaman/shellcheck/releases/download/v${SHELLCHECK_VERSION}/shellcheck-v${SHELLCHECK_VERSION}.linux.x86_64.tar.xz" \
    --output "$shellcheck_archive"
  tar -xJf "$shellcheck_archive" -C /tmp
  install_cmd=(install)
  if [[ "$EUID" -ne 0 ]]; then
    install_cmd=(sudo install)
  fi
  "${install_cmd[@]}" "/tmp/shellcheck-v${SHELLCHECK_VERSION}/shellcheck" /usr/local/bin/shellcheck
fi

if [[ "$PROFILE" == "ci" ]]; then
  PIO_VENV="${V1_PIO_VENV:-$HOME/.platformio/penv}"
  "$PYTHON_CMD" -m venv --copies "$PIO_VENV"
  "$PIO_VENV/bin/python" -m pip install \
    "${PYTHON_PACKAGES[@]}"
  VALIDATION_PYTHON="$PIO_VENV/bin/python"
  export PATH="$PIO_VENV/bin:$PATH"
  if [[ -n "${GITHUB_PATH:-}" ]]; then
    printf '%s\n' "$PIO_VENV/bin" >> "$GITHUB_PATH"
  fi
else
  "$PYTHON_CMD" -m pip install "${PYTHON_PACKAGES[@]}"
  (cd "$ROOT_DIR/interface" && npm ci)
fi

MANIFEST_PATH="${V1_VALIDATION_MANIFEST:-$ROOT_DIR/.artifacts/linux-validation-environment.txt}"
"$VALIDATION_PYTHON" "$ROOT_DIR/scripts/check_linux_validation_environment.py" \
  "$PROFILE" --root "$ROOT_DIR" --manifest "$MANIFEST_PATH"
