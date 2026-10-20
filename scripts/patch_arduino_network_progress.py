"""Bound synchronous socket stalls and feed enrolled tasks on byte progress.

NetworkClient's nested read/write loops can own the maintenance task longer
than its five-second watchdog. delay/yield do not reset that task's watchdog.
All WebServer and project streaming paths share these two transport methods.
Only actual byte progress feeds the watchdog; a stalled peer is disconnected
within 2.5 seconds, including after a partial transfer and across clock wrap.
"""
from pathlib import Path
import hashlib

UPSTREAM_SHA256 = "58c662b52a6fddce366a1b592b5720b33a92d81f2881955f4e81ad2f0f933b15"
PATCHED_SHA256 = "cd81a93a1531db0c85f3f99db7f7d5a96ec1e5c7bc2c5d0476886710257fdbba"
INCLUDE = '#include <errno.h>'
PROGRESS = '''#include <errno.h>
#include <esp_task_wdt.h>

// v1simple-network-progress-v1: never extend a stalled peer's lifetime.
static constexpr uint32_t v1simpleNetworkIdleMs = 2500;
static void v1simpleNetworkProgress() {
  if (esp_task_wdt_status(nullptr) == ESP_OK) {
    esp_task_wdt_reset();
  }
}'''
WRITE_START = '''  size_t bytesRemaining = size;

  if (!_connected'''
WRITE_START_PATCHED = '''  size_t bytesRemaining = size;
  uint32_t lastProgress = millis();

  if (!_connected'''
WRITE_WAIT = '''  while (retry) {
    //use select'''
WRITE_WAIT_PATCHED = '''  while (retry) {
    const uint32_t idle = static_cast<uint32_t>(millis() - lastProgress);
    if (idle >= v1simpleNetworkIdleMs) {
      stop();
      break;
    }
    //use select'''
WRITE_TIMEOUT = '    tv.tv_usec = WIFI_CLIENT_SELECT_TIMEOUT_US;'
WRITE_TIMEOUT_PATCHED = '''    tv.tv_usec = std::min<uint32_t>(WIFI_CLIENT_SELECT_TIMEOUT_US,
                                    (v1simpleNetworkIdleMs - idle) * 1000u);'''
WRITE_PROGRESS = '''      if (res > 0) {
        totalBytesSent += res;'''
WRITE_PROGRESS_PATCHED = '''      if (res > 0) {
        lastProgress = millis();
        v1simpleNetworkProgress();
        totalBytesSent += res;'''
WRITE_ERROR = """    if (select(socketFileDescriptor + 1, NULL, &set, NULL, &tv) < 0) {
      return 0;
    }"""
WRITE_ERROR_PATCHED = """    if (select(socketFileDescriptor + 1, NULL, &set, NULL, &tv) < 0) {
      stop();
      return totalBytesSent;
    }"""
WRITE_END = '''  return totalBytesSent;
}

size_t NetworkClient::write_P'''
WRITE_END_PATCHED = '''  // A short send terminates this response; outer retries must not restart
  // another idle window after a partial transfer without fresh progress.
  if (totalBytesSent != size) stop();
  return totalBytesSent;
}

size_t NetworkClient::write_P'''
READ_START = '  int r = 0, to = millis() + getTimeout();'
READ_START_PATCHED = '''  int r = 0;
  uint32_t lastProgress = millis();
  const uint32_t idleLimit = std::min<uint32_t>(getTimeout(), v1simpleNetworkIdleMs);'''
READ_PROGRESS = '      to = millis() + getTimeout();'
READ_PROGRESS_PATCHED = '      lastProgress = millis();'
READ_TIMEOUT = '      if (millis() >= to) {'
READ_TIMEOUT_PATCHED = '''      if (static_cast<uint32_t>(millis() - lastProgress) >= idleLimit) {
        stop();'''


READ_BUFFER_END = """  return res;
}

size_t NetworkClient::readBytes"""
READ_BUFFER_END_PATCHED = """  // All receive consumers, including single-byte multipart readers and
  // bounded error drains, share this nonblocking buffer-read owner.
  if (res > 0) v1simpleNetworkProgress();
  return res;
}

size_t NetworkClient::readBytes"""

REPLACEMENTS = (
    (READ_BUFFER_END, READ_BUFFER_END_PATCHED),
    (INCLUDE, PROGRESS), (WRITE_START, WRITE_START_PATCHED),
    (WRITE_WAIT, WRITE_WAIT_PATCHED), (WRITE_TIMEOUT, WRITE_TIMEOUT_PATCHED),
    (WRITE_PROGRESS, WRITE_PROGRESS_PATCHED), (WRITE_ERROR, WRITE_ERROR_PATCHED),
    (WRITE_END, WRITE_END_PATCHED),
    (READ_START, READ_START_PATCHED), (READ_PROGRESS, READ_PROGRESS_PATCHED),
    (READ_TIMEOUT, READ_TIMEOUT_PATCHED),
)


def patch_source(source: str) -> str:
    for original, replacement in REPLACEMENTS:
        if source.count(original) != 1:
            raise RuntimeError("pinned NetworkClient source shape changed")
        source = source.replace(original, replacement, 1)
    return source


def apply(env) -> None:
    source = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32")) / "libraries/Network/src/NetworkClient.cpp"
    text = source.read_text(encoding="utf-8")
    actual = hashlib.sha256(text.encode()).hexdigest()
    if actual == PATCHED_SHA256:
        print("[patch_arduino_network_progress] already applied")
        return
    if actual != UPSTREAM_SHA256:
        raise RuntimeError(f"pinned NetworkClient identity mismatch: {actual}")
    patched = patch_source(text)
    if hashlib.sha256(patched.encode()).hexdigest() != PATCHED_SHA256:
        raise RuntimeError("generated NetworkClient identity mismatch")
    source.write_text(patched, encoding="utf-8")
    print("[patch_arduino_network_progress] applied")


if "Import" in globals():
    Import("env")  # noqa: F821
    apply(env)  # noqa: F821
