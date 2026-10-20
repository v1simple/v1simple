"""Harden the pinned Arduino WebServer body readers.

The upstream 3.3.12 implementation grows its buffer by every currently
available byte, even when that exceeds the remaining Content-Length, and then
constructs the request argument as a NUL-terminated String.  This deterministic
patch clamps every read to the declared remainder and preserves the explicit
byte length (including embedded NUL) for the handler's exact JSON validator.
"""

Import("env")  # noqa: F821  (SCons construction environment)

from pathlib import Path


MARKER = "v1simple-webserver-exact-body-v2"
CONTRACT_SYMBOL = "v1simple_webserver_exact_body_contract"
UPSTREAM_SHA256 = "c24f1e65b4ce3fb7ab868f6bca7aab7af5dcdfd1ca81ce9716752a922e52ce78"
PREVIOUS_SHA256 = "4542ba8a04f50c35eef8939149572ae8f60338d073139cb4ab93e186d0a10aed"
PROGRESS_SHA256 = "2e15b976608499e7e99152e9c8b03e55824908f4dba1de96b2b0e732f8d6a7ea"
PATCHED_SHA256 = "2fc6f58cf8f1a6c099a1865cc1c530327a69ab9e2804afaa04694f78abb21452"

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
      // Do not let a response restart an idle window after a stalled body.
      client.stop();
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

READ_PREVIOUS = READ_PATCHED.replace(
    "      // Do not let a response restart an idle window after a stalled body.\n      client.stop();\n", "", 1
)

STRING_UPSTREAM = "          arg.value = String(plainBuf);"
STRING_PATCHED = (
    "          // v1simple-webserver-exact-body-v1: retain embedded NUL and the exact Content-Length.\n"
    "          arg.value = String(plainBuf, plainLength);"
)

INCLUDE_UPSTREAM = '#include "detail/mimetable.h"'
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
FUNCTION_PREVIOUS = """static char *readBytesWithTimeout(NetworkClient &client, size_t maxLength, size_t &dataLength, int timeout_ms) {
  // Keep the final-ELF contract marker live in the same translation unit and
  // section graph as the hardened reader.
  asm volatile("" : : "r"(&v1simple_webserver_exact_body_contract));
  char *buf = nullptr;
  dataLength = 0;"""
FUNCTION_PATCHED = """static char *readBytesWithTimeout(NetworkClient &client, size_t maxLength, size_t &dataLength, int timeout_ms) {
  // Keep the final-ELF contract marker live in the same translation unit and
  // section graph as the hardened reader.
  asm volatile("" : : "r"(&v1simple_webserver_exact_body_contract));
  // Bound an idle plain-body wait below the enrolled task watchdog.
  timeout_ms = std::min(timeout_ms, 2500);
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


LINE_UPSTREAM = """static LineStatus readLineWithLimit(NetworkClient &client, String &line, size_t maxLength, unsigned long phaseStart = 0, unsigned long phaseBudget = 0) {
  line = "";
  const unsigned long idleTimeout = client.getTimeout();
  const unsigned long lineStart = millis();
  unsigned long lastActivity = lineStart;

  while (millis() - lastActivity < idleTimeout) {
#if WEBSERVER_MAX_LINE_WAIT > 0
    if (millis() - lineStart >= WEBSERVER_MAX_LINE_WAIT) {
      log_e("Protocol line still incomplete after %u ms", (unsigned)WEBSERVER_MAX_LINE_WAIT);
      return LineStatus::TimedOut;
    }
#endif
    if (phaseBudget && millis() - phaseStart >= phaseBudget) {
      log_e("Request headers still incomplete after %u ms", (unsigned)phaseBudget);
      return LineStatus::TimedOut;
    }

    int c = client.read();
    if (c < 0) {
      yield();
      continue;
    }
    lastActivity = millis();
    if (c == '\\r') {
      // Consume the paired LF, waiting for it as readStringUntil('\\n') would.
      while (millis() - lastActivity < idleTimeout) {
        int next = client.peek();
        if (next < 0) {
          yield();
          continue;
        }
        if (next == '\\n') {
          client.read();
        }
        break;
      }
      return LineStatus::Ok;
    }
    if (line.length() >= maxLength) {
      log_e("Protocol line longer than %u bytes", (unsigned)maxLength);
      return LineStatus::TooLong;
    }
    line += (char)c;
  }
  // The peer stopped sending part-way through a line, so there is no complete
  // request to act on.
  return LineStatus::TimedOut;
}"""
LINE_PATCHED = """static LineStatus readLineWithLimit(NetworkClient &client, String &line, size_t maxLength, unsigned long phaseStart = 0, unsigned long phaseBudget = 0) {
  line = "";
  const unsigned long idleTimeout = std::min<uint32_t>(client.getTimeout(), 2500u);
  const unsigned long lineStart = millis();
  unsigned long lastActivity = lineStart;

  while (millis() - lastActivity < idleTimeout) {
#if WEBSERVER_MAX_LINE_WAIT > 0
    if (millis() - lineStart >= WEBSERVER_MAX_LINE_WAIT) {
      log_e("Protocol line still incomplete after %u ms", (unsigned)WEBSERVER_MAX_LINE_WAIT);
      client.stop();
      return LineStatus::TimedOut;
    }
#endif
    if (phaseBudget && millis() - phaseStart >= phaseBudget) {
      log_e("Request headers still incomplete after %u ms", (unsigned)phaseBudget);
      client.stop();
      return LineStatus::TimedOut;
    }

    int c = client.read();
    if (c < 0) {
      yield();
      continue;
    }
    lastActivity = millis();
    if (c == '\\r') {
      // Consume the paired LF, waiting for it as readStringUntil('\\n') would.
      while (millis() - lastActivity < idleTimeout) {
        int next = client.peek();
        if (next < 0) {
          yield();
          continue;
        }
        if (next == '\\n') {
          client.read();
        }
        break;
      }
      if (millis() - lastActivity >= idleTimeout) {
        client.stop();
        return LineStatus::TimedOut;
      }
      return LineStatus::Ok;
    }
    if (line.length() >= maxLength) {
      log_e("Protocol line longer than %u bytes", (unsigned)maxLength);
      return LineStatus::TooLong;
    }
    line += (char)c;
  }
  // The peer stopped sending part-way through a line, so there is no complete
  // request to act on.
  client.stop();
  return LineStatus::TimedOut;
}"""
UPLOAD_UPSTREAM = """int WebServer::_uploadReadByte(NetworkClient &client) {
  int res = client.read();

  if (res < 0) {
    // keep trying until you either read a valid byte or timeout
    const unsigned long startMillis = millis();
    const long timeoutIntervalMillis = client.getTimeout();
    bool timedOut = false;
    for (;;) {
      if (!client.connected()) {
        return -1;
      }
      // loosely modeled after blinkWithoutDelay pattern
      while (!timedOut && !client.available() && client.connected()) {
        delay(2);
        timedOut = (millis() - startMillis) >= timeoutIntervalMillis;
      }

      res = client.read();
      if (res >= 0) {
        return res;  // exit on a valid read
      }
      // NOTE: it is possible to get here and have all of the following
      //       assertions hold true
      //
      //       -- client.available() > 0
      //       -- client.connected == true
      //       -- res == -1
      //
      //       a simple retry strategy overcomes this which is to say the
      //       assertion is not permanent, but the reason that this works
      //       is elusive, and possibly indicative of a more subtle underlying
      //       issue

      timedOut = (millis() - startMillis) >= timeoutIntervalMillis;
      if (timedOut) {
        return res;  // exit on a timeout
      }
    }
  }

  return res;
}"""
UPLOAD_PATCHED = """int WebServer::_uploadReadByte(NetworkClient &client) {
  int res = client.read();

  if (res < 0) {
    // keep trying until you either read a valid byte or timeout
    const unsigned long startMillis = millis();
    const uint32_t timeoutIntervalMillis = std::min<uint32_t>(client.getTimeout(), 2500u);
    bool timedOut = false;
    for (;;) {
      if (!client.connected()) {
        return -1;
      }
      // loosely modeled after blinkWithoutDelay pattern
      while (!timedOut && !client.available() && client.connected()) {
        delay(2);
        timedOut = (millis() - startMillis) >= timeoutIntervalMillis;
      }

      res = client.read();
      if (res >= 0) {
        return res;  // exit on a valid read
      }
      // NOTE: it is possible to get here and have all of the following
      //       assertions hold true
      //
      //       -- client.available() > 0
      //       -- client.connected == true
      //       -- res == -1
      //
      //       a simple retry strategy overcomes this which is to say the
      //       assertion is not permanent, but the reason that this works
      //       is elusive, and possibly indicative of a more subtle underlying
      //       issue

      timedOut = (millis() - startMillis) >= timeoutIntervalMillis;
      if (timedOut) {
        client.stop();
        return res;  // exit on a timeout
      }
    }
  }

  return res;
}"""

FORM_UPSTREAM = """bool WebServer::_parseForm(NetworkClient &client, const String &boundary, uint32_t len) {
  (void)len;"""
FORM_PATCHED = """bool WebServer::_parseForm(NetworkClient &client, const String &boundary, uint32_t len) {
  // Match the request-header limit before using the fixed boundary buffer.
  if (boundary.length() > 70) {
    return false;
  }
  (void)len;"""
BOUNDARY_UPSTREAM = """            int fastBoundaryLen = 4 /* \\r\\n-- */ + boundary.length() + 1 /* \\0 */;
            char fastBoundary[fastBoundaryLen];"""
BOUNDARY_PATCHED = """            const int fastBoundaryLen = 4 /* \\r\\n-- */ + boundary.length() + 1 /* \\0 */;
            // The multipart boundary is at most 70 bytes; no C++ VLA is needed.
            char fastBoundary[4 + 70 + 1];"""


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
if actual == PREVIOUS_SHA256:
    text = text.replace(FUNCTION_PREVIOUS, FUNCTION_PATCHED, 1)
    text = text.replace(READ_PREVIOUS, READ_PATCHED, 1)
    text = text.replace(LINE_UPSTREAM, LINE_PATCHED, 1)
    text = text.replace(UPLOAD_UPSTREAM, UPLOAD_PATCHED, 1)
    if hashlib.sha256(text.encode("utf-8")).hexdigest() != PROGRESS_SHA256:
        fail("previous body patch upgrade mismatch")
    actual = PROGRESS_SHA256
if actual == PROGRESS_SHA256:
    text = text.replace(FORM_UPSTREAM, FORM_PATCHED, 1)
    text = text.replace(BOUNDARY_UPSTREAM, BOUNDARY_PATCHED, 1)
    if hashlib.sha256(text.encode("utf-8")).hexdigest() != PATCHED_SHA256:
        fail("bounded multipart boundary upgrade mismatch")
    source.write_text(text, encoding="utf-8")
    actual = PATCHED_SHA256
if actual == PATCHED_SHA256:
    for required in (READ_PATCHED, STRING_PATCHED, FUNCTION_PATCHED, RAW_PATCHED, LINE_PATCHED,
                     UPLOAD_PATCHED, FORM_PATCHED, BOUNDARY_PATCHED, CONTRACT_SYMBOL):
        if required not in text:
            fail("existing WebServer body patch is incomplete")
    print("[patch_arduino_webserver_body] already applied")
elif actual == UPSTREAM_SHA256:
    for upstream in (READ_UPSTREAM, STRING_UPSTREAM, INCLUDE_UPSTREAM, FUNCTION_UPSTREAM, RAW_UPSTREAM,
                     LINE_UPSTREAM, UPLOAD_UPSTREAM, FORM_UPSTREAM, BOUNDARY_UPSTREAM):
        if text.count(upstream) != 1:
            fail("pinned Parsing.cpp does not contain the unique expected body-reader shape")
    text = text.replace(INCLUDE_UPSTREAM, INCLUDE_PATCHED, 1)
    text = text.replace(FUNCTION_UPSTREAM, FUNCTION_PATCHED, 1)
    text = text.replace(READ_UPSTREAM, READ_PATCHED, 1)
    text = text.replace(STRING_UPSTREAM, STRING_PATCHED, 1)
    text = text.replace(RAW_UPSTREAM, RAW_PATCHED, 1)
    text = text.replace(LINE_UPSTREAM, LINE_PATCHED, 1)
    text = text.replace(UPLOAD_UPSTREAM, UPLOAD_PATCHED, 1)
    text = text.replace(FORM_UPSTREAM, FORM_PATCHED, 1)
    text = text.replace(BOUNDARY_UPSTREAM, BOUNDARY_PATCHED, 1)
    patched = hashlib.sha256(text.encode("utf-8")).hexdigest()
    if patched != PATCHED_SHA256:
        fail(f"generated patched Parsing.cpp identity mismatch: expected {PATCHED_SHA256}, got {patched}")
    source.write_text(text, encoding="utf-8")
    print("[patch_arduino_webserver_body] applied")
else:
    fail(f"pinned Parsing.cpp identity mismatch: expected pristine or patched source, got {actual}")
