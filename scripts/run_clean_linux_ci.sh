#!/bin/bash
# Reproduce the GitHub Ubuntu gate from an exact clean local commit.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="docker.io/library/ubuntu@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517"
NODE_VERSION="22.23.2"
NODE_SHA256="d60acfe00a2932254bb0ad20e01b0d74397a0875595de719654b214f4b03f307"
ENGINE=""
RUN_FULL_GATE=true

usage() {
  cat <<'EOF'
Usage: scripts/run_clean_linux_ci.sh [options]

  --engine docker|podman  Select the available container engine.
  --image IMAGE           Override the pinned Ubuntu 24.04 image.
  --preflight-only        Bootstrap and verify prerequisites without the full gate.
  --help                  Show this message.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --engine)
      ENGINE="${2:-}"
      shift 2
      ;;
    --image)
      IMAGE="${2:-}"
      shift 2
      ;;
    --preflight-only)
      RUN_FULL_GATE=false
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if ! python3 "$ROOT_DIR/scripts/check_tracked_source_state.py" --repo "$ROOT_DIR"; then
  echo "[clean-linux] refusing to run anything other than an exact clean commit." >&2
  exit 1
fi

if [[ -z "$ENGINE" ]]; then
  if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    ENGINE=docker
  elif command -v podman >/dev/null 2>&1 && podman info >/dev/null 2>&1; then
    ENGINE=podman
  else
    echo "[clean-linux] no running Docker or Podman engine is available." >&2
    exit 1
  fi
fi
if [[ "$ENGINE" != "docker" && "$ENGINE" != "podman" ]]; then
  echo "[clean-linux] --engine must be docker or podman." >&2
  exit 2
fi
if ! "$ENGINE" info >/dev/null 2>&1; then
  echo "[clean-linux] $ENGINE is installed but its engine is unavailable." >&2
  exit 1
fi

SOURCE_SHA="$(git -C "$ROOT_DIR" rev-parse HEAD)"
SOURCE_ORIGIN="$(git -C "$ROOT_DIR" remote get-url origin)"
RUN_STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUTPUT_DIR="$ROOT_DIR/.artifacts/clean-linux-${SOURCE_SHA:0:12}-$RUN_STAMP"
mkdir -p "$OUTPUT_DIR"
printf 'source_commit=%s\ncontainer_image=%s\nengine=%s\n' \
  "$SOURCE_SHA" "$IMAGE" "$ENGINE" > "$OUTPUT_DIR/launch.txt"

echo "[clean-linux] source: $SOURCE_SHA"
echo "[clean-linux] image:  $IMAGE"
echo "[clean-linux] output: $OUTPUT_DIR"

"$ENGINE" run --rm --platform linux/amd64 --pull=always \
  -e "SOURCE_SHA=$SOURCE_SHA" \
  -e "SOURCE_ORIGIN=$SOURCE_ORIGIN" \
  -e "V1_CLEAN_LINUX_IMAGE=$IMAGE" \
  -e "V1_NODE_VERSION=$NODE_VERSION" \
  -e "V1_NODE_SHA256=$NODE_SHA256" \
  -e "RUN_FULL_GATE=$RUN_FULL_GATE" \
  -v "$ROOT_DIR:/source:ro" \
  -v "$OUTPUT_DIR:/host-artifacts" \
  "$IMAGE" /bin/bash -lc '
    set -euo pipefail
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install --no-install-recommends --yes \
      build-essential ca-certificates curl git python3 python3-pip python3-venv xz-utils
    node_archive="node-v${V1_NODE_VERSION}-linux-x64.tar.xz"
    curl --fail --location --silent --show-error \
      "https://nodejs.org/dist/v${V1_NODE_VERSION}/${node_archive}" \
      --output "/tmp/${node_archive}"
    printf "%s  %s\n" "$V1_NODE_SHA256" "/tmp/${node_archive}" | sha256sum --check --strict
    tar -xJf "/tmp/${node_archive}" -C /usr/local --strip-components=1
    git clone --no-hardlinks /source /work/repo
    test "$(git -C /work/repo rev-parse HEAD)" = "$SOURCE_SHA"
    git -C /work/repo remote set-url origin "$SOURCE_ORIGIN"
    cd /work/repo
    retain_reports() {
      if [[ -d .artifacts/test_reports ]]; then
        cp -R .artifacts/test_reports /host-artifacts/test_reports
      fi
    }
    trap retain_reports EXIT
    export V1_VALIDATION_MANIFEST=/host-artifacts/linux-validation-environment.txt
    set -o pipefail
    ./scripts/bootstrap_linux_validation.sh ci 2>&1 | tee /host-artifacts/bootstrap.log
    if [[ "$RUN_FULL_GATE" == "true" ]]; then
      PLATFORMIO_RUN_JOBS=1 ./scripts/ci-test.sh 2>&1 | tee /host-artifacts/ci-test.log
    fi
  '

echo "[clean-linux] completed for $SOURCE_SHA"
