#!/usr/bin/env bash
# Source after PIO_CMD is set. PlatformIO's Python HTTP client can fail on
# macOS when it cannot find a CA bundle, even though the same package download
# works with curl. Prefer certifi from the active PlatformIO interpreter and
# export the variables used by requests/urllib before any pio network command.

_platformio_ca_fail() {
  echo "[toolchain] $*" >&2
  return 1 2>/dev/null || exit 1
}

if [[ "${PLATFORMIO_SKIP_CA_BOOTSTRAP:-0}" == "1" ]]; then
  return 0 2>/dev/null || exit 0
fi

if [[ -n "${REQUESTS_CA_BUNDLE:-}" && -f "${REQUESTS_CA_BUNDLE}" ]]; then
  export SSL_CERT_FILE="${SSL_CERT_FILE:-$REQUESTS_CA_BUNDLE}"
  return 0 2>/dev/null || exit 0
fi

if [[ -n "${SSL_CERT_FILE:-}" && -f "${SSL_CERT_FILE}" ]]; then
  export REQUESTS_CA_BUNDLE="${REQUESTS_CA_BUNDLE:-$SSL_CERT_FILE}"
  return 0 2>/dev/null || exit 0
fi

if [[ -n "${REQUESTS_CA_BUNDLE:-}" && ! -f "${REQUESTS_CA_BUNDLE}" ]]; then
  echo "[toolchain] ignoring missing REQUESTS_CA_BUNDLE=${REQUESTS_CA_BUNDLE}" >&2
fi
if [[ -n "${SSL_CERT_FILE:-}" && ! -f "${SSL_CERT_FILE}" ]]; then
  echo "[toolchain] ignoring missing SSL_CERT_FILE=${SSL_CERT_FILE}" >&2
fi

_platformio_source_path="${BASH_SOURCE:-$0}"
_platformio_script_dir="$(cd "$(dirname "${_platformio_source_path}")" && pwd)"
_platformio_root_dir="$(cd "${_platformio_script_dir}/.." && pwd)"
_platformio_pio_path=""

if [[ -n "${PIO_CMD:-}" ]]; then
  if [[ "${PIO_CMD}" == */* ]]; then
    if [[ -x "${PIO_CMD}" ]]; then
      _platformio_pio_path="${PIO_CMD}"
    fi
  else
    _platformio_pio_path="$(command -v "${PIO_CMD}" 2>/dev/null || true)"
  fi
fi

declare -a _platformio_python_candidates=()
_platformio_add_python_candidate() {
  local candidate="$1"
  [[ -n "${candidate}" ]] || return 0
  local existing
  if ((${#_platformio_python_candidates[@]} > 0)); then
    for existing in "${_platformio_python_candidates[@]}"; do
      [[ "${existing}" == "${candidate}" ]] && return 0
    done
  fi
  _platformio_python_candidates+=("${candidate}")
}

if [[ -n "${_platformio_pio_path}" ]]; then
  _platformio_pio_dir="$(cd "$(dirname "${_platformio_pio_path}")" && pwd)"
  [[ -x "${_platformio_pio_dir}/python" ]] && _platformio_add_python_candidate "${_platformio_pio_dir}/python"

  if IFS= read -r _platformio_pio_shebang < "${_platformio_pio_path}" 2>/dev/null; then
    if [[ "${_platformio_pio_shebang}" == '#!'*python* ]]; then
      _platformio_shebang_cmd="${_platformio_pio_shebang#\#!}"
      read -r _platformio_shebang_first _platformio_shebang_second _platformio_shebang_rest <<< "${_platformio_shebang_cmd}"
      if [[ "${_platformio_shebang_first}" == "/usr/bin/env" || "${_platformio_shebang_first}" == "env" ]]; then
        _platformio_add_python_candidate "${_platformio_shebang_second}"
      else
        _platformio_add_python_candidate "${_platformio_shebang_first}"
      fi
    fi
  fi
fi

_platformio_add_python_candidate "${_platformio_root_dir}/.artifacts/pio-core-6.1.19/bin/python"
_platformio_add_python_candidate "python3"

_platformio_ca_bundle=""
for _platformio_python in "${_platformio_python_candidates[@]}"; do
  if [[ "${_platformio_python}" == */* ]]; then
    [[ -x "${_platformio_python}" ]] || continue
  elif ! command -v "${_platformio_python}" >/dev/null 2>&1; then
    continue
  fi

  if _platformio_candidate_bundle="$(
    "${_platformio_python}" - <<'PY' 2>/dev/null
import os
try:
    import certifi
except ImportError:
    raise SystemExit(1)
path = certifi.where()
if not os.path.isfile(path):
    raise SystemExit(1)
print(path)
PY
  )" && [[ -f "${_platformio_candidate_bundle}" ]]; then
    _platformio_ca_bundle="${_platformio_candidate_bundle}"
    break
  fi
done

if [[ -z "${_platformio_ca_bundle}" ]]; then
  _platformio_ca_fail "Unable to find certifi for PlatformIO TLS downloads. Install/repair PlatformIO or run: python3 -m pip install certifi"
fi

export SSL_CERT_FILE="${_platformio_ca_bundle}"
export REQUESTS_CA_BUNDLE="${_platformio_ca_bundle}"
echo "[toolchain] PlatformIO CA bundle: ${_platformio_ca_bundle}" >&2

unset _platformio_source_path _platformio_script_dir _platformio_root_dir _platformio_pio_path _platformio_pio_dir
unset _platformio_pio_shebang _platformio_shebang_cmd _platformio_shebang_first _platformio_shebang_second _platformio_shebang_rest
unset _platformio_python _platformio_candidate_bundle _platformio_ca_bundle
unset -f _platformio_ca_fail _platformio_add_python_candidate
