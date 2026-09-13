"""Harden the pinned Arduino WebServer body readers.

The upstream 3.3.11 implementation grows its buffer by every currently
available byte, even when that exceeds the remaining Content-Length, and then
constructs the request argument as a NUL-terminated String.  This deterministic
patch clamps every read to the declared remainder and preserves the explicit
byte length (including embedded NUL) for the handler's exact JSON validator.
"""

Import("env")  # noqa: F821  (SCons construction environment)

from pathlib import Path


MARKER = "v1simple-webserver-exact-body-v2"
CONTRACT_SYMBOL = "v1simple_webserver_exact_body_contract"
UPSTREAM_SHA256 = "522a46a1b8bed19b5482b65eb96bb87fe068c937c4516d17111179b2e8b88adc"
V1_PATCHED_SHA256 = "f509fbaac1c776cbcbd3c6040d242d85d95c7d3435c8ed80bcef32890580bc01"
BROKEN_V2_PATCHED_SHA256 = "864bd3c98b147e267a8176da81afb1a122fe139bdaacc0c0fa578810bb79d406"
WARNINGFUL_PATCHED_SHA256 = "c43c6827b6ccdf198d6150bddf0b627683e4bdd6d51e957e03df5e1b7ad0db52"
PATCHED_SHA256 = "74387b5923e880dbd09198e6d3521352b4f1b6cc196a10021862ed466a8cd708"

READ_UPSTREAM = """    if (!newLength) {
      break;
    }
    if (!buf) {
      buf = (char *)malloc(newLength + 1);
      if (!buf) {
        return nullptr;
      }
    } else {
      char *newBuf = (char *)realloc(buf, dataLength + newLength + 1);
      if (!newBuf) {
        free(buf);
        return nullptr;
      }
      buf = newBuf;
    }
    client.readBytes(buf + dataLength, newLength);
    dataLength += newLength;
    buf[dataLength] = '\\0';"""

READ_PATCHED = """    if (!newLength) {
      break;
    }
    // v1simple-webserver-exact-body-v1: never read or allocate beyond the
    // declared remainder.  Preserve the actual short-read count so an
    // incomplete body cannot be reported as complete.
    newLength = std::min(newLength, maxLength - dataLength);
    if (!buf) {
      buf = (char *)malloc(newLength + 1);
      if (!buf) {
        return nullptr;
      }
    } else {
      char *newBuf = (char *)realloc(buf, dataLength + newLength + 1);
      if (!newBuf) {
        free(buf);
        return nullptr;
      }
      buf = newBuf;
    }
    const size_t bytesRead = client.readBytes(buf + dataLength, newLength);
    dataLength += bytesRead;
    buf[dataLength] = '\\0';
    if (bytesRead != newLength) {
      break;
    }"""

STRING_UPSTREAM = "          arg.value = String(plainBuf);"
STRING_BROKEN_PATCHED = (
    "          // v1simple-webserver-exact-body-v1: retain embedded NUL and the exact Content-Length.\\n"
    "          arg.value = String(plainBuf, plainLength);"
)
STRING_PATCHED = (
    "          // v1simple-webserver-exact-body-v1: retain embedded NUL and the exact Content-Length.\n"
    "          arg.value = String(plainBuf, plainLength);"
)

INCLUDE_UPSTREAM = '#include "detail/mimetable.h"'
INCLUDE_V1_PATCHED = """#include "detail/mimetable.h"

// Final-ELF qualification marker for the project-owned body-ingress patch.
extern "C" const uint32_t v1simple_webserver_exact_body_contract
  __attribute__((used, retain)) = 0x56314231u;"""
INCLUDE_WARNINGFUL_PATCHED = """#include "detail/mimetable.h"

// Final-ELF qualification marker for the project-owned body-ingress patch.
extern "C" const uint32_t v1simple_webserver_exact_body_contract
  __attribute__((used, retain)) = 0x56314232u;"""
INCLUDE_PATCHED = """#include "detail/mimetable.h"

// Final-ELF qualification marker for the project-owned body-ingress patch.
// The live reader's address reference retains this object through section GC;
// `used` prevents compiler elimination without relying on an unsupported
// target-toolchain `retain` attribute.
extern "C" const uint32_t v1simple_webserver_exact_body_contract
  __attribute__((used)) = 0x56314232u;"""

FUNCTION_UPSTREAM = """static char *readBytesWithTimeout(NetworkClient &client, size_t maxLength, size_t &dataLength, int timeout_ms) {
  char *buf = nullptr;
  dataLength = 0;"""
FUNCTION_PATCHED = """static char *readBytesWithTimeout(NetworkClient &client, size_t maxLength, size_t &dataLength, int timeout_ms) {
  // Keep the final-ELF contract marker live in the same translation unit and
  // section graph as the hardened reader.
  asm volatile("" : : "r"(&v1simple_webserver_exact_body_contract));
  char *buf = nullptr;
  dataLength = 0;"""

RAW_UPSTREAM = """    if (!isForm && _currentHandler && _currentHandler->canRaw(*this, _currentUri)) {
      log_v("Parse raw");
      _currentRaw.reset(new HTTPRaw());
      _currentRaw->status = RAW_START;
      _currentRaw->totalSize = 0;
      _currentRaw->currentSize = 0;
      log_v("Start Raw");
      _currentHandler->raw(*this, _currentUri, *_currentRaw);
      _currentRaw->status = RAW_WRITE;

      while (_currentRaw->totalSize < (size_t)_clientContentLength) {
        size_t read_len = std::min((size_t)_clientContentLength - _currentRaw->totalSize, (size_t)HTTP_RAW_BUFLEN);
        _currentRaw->currentSize = client.readBytes(_currentRaw->buf, read_len);
        _currentRaw->totalSize += _currentRaw->currentSize;
        if (_currentRaw->currentSize == 0) {
          _currentRaw->status = RAW_ABORTED;
          _currentHandler->raw(*this, _currentUri, *_currentRaw);
          return false;
        }
        _currentHandler->raw(*this, _currentUri, *_currentRaw);
      }
      _currentRaw->status = RAW_END;
      _currentHandler->raw(*this, _currentUri, *_currentRaw);
      log_v("Finish Raw");
    } else if (!isForm) {"""

RAW_PATCHED = """    // v1simple-webserver-exact-body-v2: a registered project raw handler
    // takes precedence over the framework multipart/urlencoded parsers.  It
    // may pin the read length to the allocation-free preflight result through
    // the exact contract signal, so the heap-backed header reparse is never
    // authoritative for the number of bytes consumed.
    if (_currentHandler && _currentHandler->canRaw(*this, _currentUri)) {
      log_v("Parse raw");
      _currentRaw.reset(new HTTPRaw());
      _currentRaw->status = RAW_START;
      _currentRaw->totalSize = 0;
      _currentRaw->currentSize = 0;
      _currentRaw->data = nullptr;
      log_v("Start Raw");
      _currentHandler->raw(*this, _currentUri, *_currentRaw);
      size_t rawExpectedLength = (size_t)_clientContentLength;
      if ((uintptr_t)_currentRaw->data == 0x56314232u) {
        rawExpectedLength = _currentRaw->currentSize;
      }
      _currentRaw->currentSize = 0;
      _currentRaw->status = RAW_WRITE;

      while (_currentRaw->totalSize < rawExpectedLength) {
        size_t read_len = std::min(rawExpectedLength - _currentRaw->totalSize, (size_t)HTTP_RAW_BUFLEN);
        _currentRaw->currentSize = client.readBytes(_currentRaw->buf, read_len);
        _currentRaw->totalSize += _currentRaw->currentSize;
        if (_currentRaw->currentSize == 0) {
          _currentRaw->status = RAW_ABORTED;
          _currentHandler->raw(*this, _currentUri, *_currentRaw);
          return false;
        }
        _currentHandler->raw(*this, _currentUri, *_currentRaw);
      }
      _currentRaw->status = RAW_END;
      _currentHandler->raw(*this, _currentUri, *_currentRaw);
      log_v("Finish Raw");
    } else if (!isForm) {"""


def fail(message: str) -> None:
    print(f"[patch_arduino_webserver_body] ERROR: {message}")
    env.Exit(1)  # noqa: F821


source = (
    Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32") or "")  # noqa: F821
    / "libraries"
    / "WebServer"
    / "src"
    / "Parsing.cpp"
)
if not source.is_file():
    fail(f"missing pinned WebServer source: {source}")

text = source.read_text(encoding="utf-8")
import hashlib

actual = hashlib.sha256(text.encode("utf-8")).hexdigest()
if actual == PATCHED_SHA256:
    for required in (READ_PATCHED, STRING_PATCHED, FUNCTION_PATCHED, RAW_PATCHED, CONTRACT_SYMBOL):
        if required not in text:
            fail("existing WebServer body patch is incomplete")
    print("[patch_arduino_webserver_body] already applied")
elif actual in (
    UPSTREAM_SHA256,
    V1_PATCHED_SHA256,
    BROKEN_V2_PATCHED_SHA256,
    WARNINGFUL_PATCHED_SHA256,
):
    if actual in (UPSTREAM_SHA256, V1_PATCHED_SHA256) and text.count(RAW_UPSTREAM) != 1:
        fail("pinned Parsing.cpp does not contain the unique expected raw-reader shape")
    if actual == UPSTREAM_SHA256:
        for upstream in (READ_UPSTREAM, STRING_UPSTREAM, INCLUDE_UPSTREAM, FUNCTION_UPSTREAM):
            if text.count(upstream) != 1:
                fail("pinned Parsing.cpp does not contain the unique expected body-reader shape")
        text = text.replace(INCLUDE_UPSTREAM, INCLUDE_PATCHED, 1)
        text = text.replace(FUNCTION_UPSTREAM, FUNCTION_PATCHED, 1)
        text = text.replace(READ_UPSTREAM, READ_PATCHED, 1)
        text = text.replace(STRING_UPSTREAM, STRING_PATCHED, 1)
    elif actual == V1_PATCHED_SHA256:
        if text.count(INCLUDE_V1_PATCHED) != 1:
            fail("v1-patched Parsing.cpp marker is missing")
        text = text.replace(INCLUDE_V1_PATCHED, INCLUDE_PATCHED, 1)
        if text.count(STRING_BROKEN_PATCHED) != 1:
            fail("v1-patched Parsing.cpp body assignment is missing")
        text = text.replace(STRING_BROKEN_PATCHED, STRING_PATCHED, 1)
    elif actual == BROKEN_V2_PATCHED_SHA256:
        if text.count(INCLUDE_WARNINGFUL_PATCHED) != 1:
            fail("v2-patched Parsing.cpp warningful marker is missing")
        text = text.replace(INCLUDE_WARNINGFUL_PATCHED, INCLUDE_PATCHED, 1)
        if text.count(STRING_BROKEN_PATCHED) != 1:
            fail("v2-patched Parsing.cpp body assignment is missing")
        text = text.replace(STRING_BROKEN_PATCHED, STRING_PATCHED, 1)
    else:
        if text.count(INCLUDE_WARNINGFUL_PATCHED) != 1:
            fail("existing Parsing.cpp warningful marker is missing")
        text = text.replace(INCLUDE_WARNINGFUL_PATCHED, INCLUDE_PATCHED, 1)
    if actual != BROKEN_V2_PATCHED_SHA256:
        text = text.replace(RAW_UPSTREAM, RAW_PATCHED, 1)
    patched = hashlib.sha256(text.encode("utf-8")).hexdigest()
    if patched != PATCHED_SHA256:
        fail(f"generated patched Parsing.cpp identity mismatch: expected {PATCHED_SHA256}, got {patched}")
    source.write_text(text, encoding="utf-8")
    print("[patch_arduino_webserver_body] applied")
else:
    fail(f"pinned Parsing.cpp identity mismatch: expected pristine or patched source, got {actual}")
