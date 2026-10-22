#!/usr/bin/env python3
"""Compile production HTTP transfer owners against timed socket substitutes.

Exercises the pinned NetworkClient read/write loops, raw/plain/multipart ingress,
static streaming, streamed/cached JSON and framework header output. Only sockets,
files, clock and watchdog enrollment are substituted; resets must follow actual
byte progress. This proves software scheduling bounds, not radio/device timing.
"""
from __future__ import annotations

import argparse
import ast
import contextlib
from functools import lru_cache
import gzip
import hashlib
import io
import os
from pathlib import Path
import re
import runpy
import shlex
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def function(source: str, marker: str) -> str:
    if source.count(marker) != 1:
        raise AssertionError(f"expected one production owner: {marker}")
    begin = source.index(marker)
    end = source.index("{", begin) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]


@lru_cache(maxsize=None)
def patch_constants(path: Path) -> dict[str, str]:
    assignments = []
    for node in ast.parse(path.read_text()).body:
        if isinstance(node, ast.Assign):
            if any(isinstance(target, ast.Name) and target.id == "source" for target in node.targets):
                break
            assignments.append(node)
    namespace = {}
    exec(compile(ast.Module(body=assignments, type_ignores=[]), str(path), "exec"), namespace)
    return namespace


def literal(path: Path, name: str) -> str:
    return patch_constants(path)[name]


def apply_in_temporary_framework(source: str, relative: str, patch: Path) -> str:
    """Run the real SCons patch twice, without modifying the installed package."""
    with tempfile.TemporaryDirectory(prefix="v1-http-patch-") as temporary:
        framework = Path(temporary)
        target = framework / relative
        target.parent.mkdir(parents=True)
        target.write_text(source)

        class Environment:
            def PioPlatform(self):
                return self

            def get_package_dir(self, name):
                assert name == "framework-arduinoespressif32"
                return str(framework)

            def Exit(self, status):
                raise AssertionError(f"production patch failed with {status}")

        with contextlib.redirect_stdout(io.StringIO()):
            previous = None
            for _ in range(2):
                runpy.run_path(str(patch), init_globals={"Import": lambda _: None, "env": Environment()})
                result = target.read_text()
                if previous is not None:
                    assert result == previous, "production patch is not idempotent"
                previous = result
        return result


def production_sources(framework: Path, baseline: bool) -> dict[str, str]:
    network_patch = ROOT / "scripts/patch_arduino_network_progress.py"
    patch = runpy.run_path(str(network_patch))
    network_path = "libraries/Network/src/NetworkClient.cpp"
    network = (framework / network_path).read_text()
    identity = lambda value: hashlib.sha256(value.encode()).hexdigest()
    assert identity(network) in (patch["UPSTREAM_SHA256"], patch["PATCHED_SHA256"]), \
        "installed NetworkClient identity is not the pinned original or complete project patch"
    if "v1simple-network-progress-v1" in network:
        pristine = network
        # Restore larger blocks first: the read timestamp assignment also
        # appears inside the write-progress replacement.
        for before, after in sorted(patch["REPLACEMENTS"], key=lambda pair: len(pair[1]), reverse=True):
            if pristine.count(after) != 1:
                raise AssertionError("installed network patch is incomplete")
            pristine = pristine.replace(after, before, 1)
    else:
        pristine = network
    assert identity(pristine) == patch["UPSTREAM_SHA256"]
    transformed = patch["patch_source"](pristine)
    assert identity(transformed) == patch["PATCHED_SHA256"]
    assert apply_in_temporary_framework(pristine, network_path, network_patch) == transformed
    network = pristine if baseline else transformed
    body_patch = ROOT / "scripts/patch_arduino_webserver_body.py"
    parsing_path = "libraries/WebServer/src/Parsing.cpp"
    parsing = (framework / parsing_path).read_text()
    assert identity(parsing) in tuple(literal(body_patch, key) for key in
                                     ("UPSTREAM_SHA256", "PREVIOUS_SHA256", "PROGRESS_SHA256", "PATCHED_SHA256"))
    parsing = apply_in_temporary_framework(parsing, parsing_path, body_patch)
    assert identity(parsing) == literal(body_patch, "PATCHED_SHA256")
    # Exercise every supported installation/upgrade path from exact identities.
    progress = parsing
    for name in ("FORM", "BOUNDARY"):
        before, after = literal(body_patch, name + "_UPSTREAM"), literal(body_patch, name + "_PATCHED")
        assert progress.count(after) == 1
        progress = progress.replace(after, before, 1)
    assert identity(progress) == literal(body_patch, "PROGRESS_SHA256")
    assert apply_in_temporary_framework(progress, parsing_path, body_patch) == parsing
    previous = progress
    for name in ("FUNCTION", "READ"):
        before, after = literal(body_patch, name + "_PREVIOUS"), literal(body_patch, name + "_PATCHED")
        assert previous.count(after) == 1
        previous = previous.replace(after, before, 1)
    for name in ("LINE", "UPLOAD"):
        before, after = literal(body_patch, name + "_UPSTREAM"), literal(body_patch, name + "_PATCHED")
        assert previous.count(after) == 1
        previous = previous.replace(after, before, 1)
    assert identity(previous) == literal(body_patch, "PREVIOUS_SHA256")
    assert apply_in_temporary_framework(previous, parsing_path, body_patch) == parsing
    original = progress
    for name in ("READ", "STRING", "INCLUDE", "FUNCTION", "RAW", "LINE", "UPLOAD"):
        before, after = literal(body_patch, name + "_UPSTREAM"), literal(body_patch, name + "_PATCHED")
        assert original.count(after) == 1
        original = original.replace(after, before, 1)
    assert identity(original) == literal(body_patch, "UPSTREAM_SHA256")
    assert apply_in_temporary_framework(original, parsing_path, body_patch) == parsing
    plain = function(previous if baseline else parsing, "static char *readBytesWithTimeout(")
    raw = function(literal(body_patch, "RAW_PATCHED"),
                   "      while (_currentRaw->totalSize < rawExpectedLength)")
    server = (framework / "libraries/WebServer/src/WebServer.cpp").read_text()
    header = (framework / "libraries/WebServer/src/WebServer.h").read_text()
    json = (ROOT / "include/json_stream_response.h").read_text()
    # Keep baseline behavior while retaining standard C++ boundary storage so
    # the negative run reaches its runtime assertions under the same flags.
    selected_parsing = previous if baseline else parsing
    if baseline:
        for name in ("FORM", "BOUNDARY"):
            selected_parsing = selected_parsing.replace(
                literal(body_patch, name + "_UPSTREAM"), literal(body_patch, name + "_PATCHED"), 1)

    def production_branch(marker: str) -> str:
        owner = function(json, marker)
        return owner[owner.index("#else") + len("#else"):owner.index("#endif")]

    progress = patch["PROGRESS"][patch["PROGRESS"].index("// v1simple-network-progress-v1"):]
    form_limits = "\n".join(
        re.search(r"#define " + name + r"\s+\d+[^\n]*", header + "\n" + parsing)[0]
        for name in ("HTTP_UPLOAD_BUFLEN", "HTTP_MAX_POST_WAIT", "WEBSERVER_MAX_LINE_LEN",
                     "WEBSERVER_MAX_LINE_WAIT", "WEBSERVER_MAX_POST_ARG_LEN",
                     "WEBSERVER_MAX_POST_ARGS", "WEBSERVER_MAX_MULTIPART_SKIP_LINES")
    )
    return {
        "FORM_LIMITS": form_limits,
        "RAW_SIZE": re.search(r"#define HTTP_RAW_BUFLEN (\d+)", header)[1],
        "WRITE_RETRIES": re.search(r"#define WIFI_CLIENT_MAX_WRITE_RETRY\s+\((\d+)\)", pristine)[1],
        "WRITE_TIMEOUT": re.search(r"#define WIFI_CLIENT_SELECT_TIMEOUT_US\s+\((\d+)\)", pristine)[1],
        "PROGRESS": "" if baseline else progress,
        "READ": function(network, "size_t NetworkClient::readBytes(char *buffer, size_t length)"),
        "READ_BYTE": function(network, "int NetworkClient::read() {"),
        "READ_BUFFER": function(network, "int NetworkClient::read(uint8_t *buf, size_t size)"),
        "WRITE": function(network, "size_t NetworkClient::write(const uint8_t *buf, size_t size)"),
        "PLAIN": plain,
        "RAW": raw,
        # unsigned long is 32-bit on ESP32; preserve that ABI on 64-bit hosts.
        "LINE": function(selected_parsing, "static LineStatus readLineWithLimit(").replace("unsigned long", "uint32_t"),
        "UPLOAD_READ": function(selected_parsing, "int WebServer::_uploadReadByte(").replace("unsigned long", "uint32_t"),
        "UPLOAD_WRITE": function(selected_parsing, "void WebServer::_uploadWriteByte("),
        "UPLOAD_ABORT": function(selected_parsing, "bool WebServer::_parseFormUploadAborted("),
        "FORM": function(selected_parsing, "bool WebServer::_parseForm("),
        "STATIC": function((ROOT / "src/wifi_manager_helpers.cpp").read_text(), "bool streamOpenFile("),
        "EMBEDDED_STATIC": function((ROOT / "src/wifi_manager_helpers.cpp").read_text(), "bool streamEmbeddedAsset("),
        "JSON_BUFFER": function(json, "template <typename ClientT, size_t kBufferSize = 512>"),
        "JSON": production_branch("inline void sendJsonStream("),
        "CACHED": production_branch("inline void sendSerializedJson("),
        "SEND_STRING": function(server, "void WebServer::send(int code, const char *content_type, const String &content)"),
        "SEND_CHARS": function(server, "void WebServer::send(int code, const char *content_type, const char *content)"),
        "SEND_CONTENT": function(server, "void WebServer::sendContent(const char *content, size_t contentLength)"),
        "SEND_CONTENT_STRING": function(server, "void WebServer::sendContent(const String &content)"),
        "CLIENT_WRITE": function(header, "  virtual size_t _currentClientWrite(const char *b, size_t l)"),
    }


ARDUINO = r"""
#pragma once
#include <cstdint>
#include <cstddef>
#include <cctype>
#include <string>
class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const std::string& value) : std::string(value) {}
  String substring(size_t a, size_t b) const { return substr(a, b - a); }
  String substring(size_t a) const { return substr(a); }
  int indexOf(char c) const { const auto at = find(c); return at == npos ? -1 : int(at); }
  bool equalsIgnoreCase(const String& value) const {
    if (size() != value.size()) return false;
    for (size_t i = 0; i < size(); ++i)
      if (tolower(static_cast<unsigned char>((*this)[i])) != tolower(static_cast<unsigned char>(value[i])))
        return false;
    return true;
  }
  bool startsWith(const String& value) const { return rfind(value, 0) == 0; }
};
extern uint32_t clockMs;
void advanceClock(uint32_t);
inline uint32_t millis() { return clockMs; }
inline void delay(uint32_t ms) { advanceClock(ms); }
inline void yield() { advanceClock(1); }
struct SerialMock { template<class... T> void printf(const char*, T...) {} };
inline SerialMock Serial;
"""


HARNESS = r"""
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <vector>
#include <ArduinoJson.h>
#include "Arduino.h"
#include "client_write_retry.h"
#include "modules/wifi/embedded_web_assets.h"
#include "modules/wifi/wifi_maintenance_http_preflight.h"
#define log_w(...) ((void)0)
#define log_e(...) ((void)0)
#define log_v(...) ((void)0)
#define F(value) value
#define FPSTR(value) value
@FORM_LIMITS@
#define HTTP_RAW_BUFLEN @RAW_SIZE@
#define WIFI_CLIENT_MAX_WRITE_RETRY @WRITE_RETRIES@
#define WIFI_CLIENT_SELECT_TIMEOUT_US @WRITE_TIMEOUT@
static constexpr uint32_t watchdogWindow = 5000;
uint32_t clockMs = 0;
static uint64_t elapsedMs = 0, lastFeedMs = 0, maxGapMs = 0;
static size_t progressBytes = 0, fedBytes = 0, feeds = 0;
static bool enrolled = true;
static unsigned failures = 0, cases = 0;
static const char* currentCase = "";
static void check(bool ok, const char* reason) {
  if (!ok) { ++failures; fprintf(stderr, "FAIL %s: %s\n", currentCase, reason); }
}
void advanceClock(uint32_t amount) {
  clockMs += amount;
  elapsedMs += amount;
  maxGapMs = std::max(maxGapMs, elapsedMs - lastFeedMs);
}
static constexpr int ESP_OK = 0;
static int esp_task_wdt_status(void*) { return enrolled ? ESP_OK : -1; }
static int esp_task_wdt_reset() {
  check(enrolled, "an unenrolled task must not reset the watchdog");
  check(progressBytes > fedBytes, "watchdog reset without new transferred bytes");
  fedBytes = progressBytes;
  lastFeedMs = elapsedMs;
  ++feeds;
  return ESP_OK;
}
@PROGRESS@
struct Peer {
  bool connected = true, disconnectAtLimit = false, writeEagain = false, writeZero = false;
  size_t readAt = 0, readLimit = SIZE_MAX, writeLimit = SIZE_MAX, writeAt = 0;
  size_t readChunk = 512, writeChunk = 128;
  uint32_t readInterval = 50, writeInterval = 100;
  uint64_t nextWriteMs = 0;
  std::string incoming, outgoing;
};
static Peer* selectedPeer = nullptr;
static int mockSelect(int, fd_set*, fd_set* writable, fd_set*, timeval* timeout) {
  Peer& p = *selectedPeer;
  const uint32_t wait = static_cast<uint32_t>(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
  if (!p.connected) { errno = ECONNRESET; return -1; }
  if (p.writeAt >= p.writeLimit) {
    if (p.disconnectAtLimit) { p.connected = false; errno = ECONNRESET; return -1; }
    advanceClock(wait);
    if (!p.writeEagain && !p.writeZero) FD_ZERO(writable);
    return p.writeEagain || p.writeZero ? 1 : 0;
  }
  if (p.nextWriteMs > elapsedMs) {
    const uint64_t untilReady = p.nextWriteMs - elapsedMs;
    advanceClock(static_cast<uint32_t>(std::min<uint64_t>(wait, untilReady)));
    if (untilReady > wait) { FD_ZERO(writable); return 0; }
  }
  return 1;
}
static ssize_t mockSend(int, const void* data, size_t length, int) {
  Peer& p = *selectedPeer;
  if (p.writeAt >= p.writeLimit) { errno = EAGAIN; return p.writeZero ? 0 : -1; }
  const size_t sent = std::min({length, p.writeChunk, p.writeLimit - p.writeAt});
  p.outgoing.append(static_cast<const char*>(data), sent);
  p.writeAt += sent;
  progressBytes += sent;
  p.nextWriteMs = elapsedMs + p.writeInterval;
  return static_cast<ssize_t>(sent);
}
static int receiveAvailable(const Peer& peer) {
  if (!peer.connected) return 0;
  const uint64_t due = (elapsedMs / peer.readInterval + 1) * peer.readChunk;
  return static_cast<int>(std::min<uint64_t>({peer.incoming.size(), peer.readLimit, due}) - peer.readAt);
}
class ReceiveBuffer {
 public:
  std::shared_ptr<Peer> peer;
  bool error = false;
  explicit ReceiveBuffer(std::shared_ptr<Peer> value) : peer(std::move(value)) {}
  int read(uint8_t* out, size_t count) {
    if (!peer->connected || (peer->disconnectAtLimit && peer->readAt >= peer->readLimit)) {
      error = true; return -1;
    }
    const size_t n = std::min(count, static_cast<size_t>(receiveAvailable(*peer)));
    memcpy(out, peer->incoming.data() + peer->readAt, n);
    peer->readAt += n;
    progressBytes += n;
    return static_cast<int>(n);
  }
  bool failed() const { return error; }
};
class NetworkClient {
 public:
  std::shared_ptr<Peer> peer;
  std::shared_ptr<ReceiveBuffer> _rxBuffer;
  bool _connected = true;
  uint32_t _lastWriteTimeout = 5000, _lastReadTimeout = 5000, _timeout = 5000;
  explicit NetworkClient(std::shared_ptr<Peer> value)
    : peer(std::move(value)), _rxBuffer(std::make_shared<ReceiveBuffer>(peer)) {}
  int fd() { selectedPeer = peer.get(); return peer->connected ? 1 : -1; }
  void stop() { _connected = false; peer->connected = false; }
  int setSocketOption(int, char*, size_t) { return 0; }
  uint32_t getTimeout() const { return _timeout; }
  int available() const { return receiveAvailable(*peer); }
  int peek() {
    if (peer->disconnectAtLimit && peer->readAt >= peer->readLimit) {
      stop(); return -1;
    }
    return available() ? static_cast<unsigned char>(peer->incoming[peer->readAt]) : -1;
  }
  bool connected() const { return peer->connected; }
  int read(uint8_t*, size_t);
  int read();
  size_t readBytes(char*, size_t);
  size_t readBytes(uint8_t* out, size_t n) { return readBytes(reinterpret_cast<char*>(out), n); }
  size_t write(const uint8_t*, size_t);
  size_t write(const char* data, size_t n) { return write(reinterpret_cast<const uint8_t*>(data), n); }
};
@READ_BUFFER@
@READ_BYTE@
@READ@
#define select mockSelect
#define send mockSend
@WRITE@
#undef send
#undef select
extern "C" const uint32_t v1simple_webserver_exact_body_contract = 0x56314232u;
@PLAIN@
enum { RAW_WRITE, RAW_ABORTED };
struct Raw { uint8_t buf[HTTP_RAW_BUFLEN]{}; size_t totalSize = 0, currentSize = 0; int status = RAW_WRITE; };
struct RawHandler {
  std::string body;
  template<class T> void raw(T&, int, Raw& r) {
    if (r.status == RAW_WRITE) body.append(reinterpret_cast<const char*>(r.buf), r.currentSize);
  }
};
struct Receiver {
  Raw storage;
  Raw* _currentRaw = &storage;
  RawHandler handler;
  RawHandler* _currentHandler = &handler;
  int _currentUri = 0;
  bool receive(NetworkClient& client, size_t rawExpectedLength) {
    @RAW@
    return true;
  }
};
struct File {
  std::string data;
  size_t at = 0;
  bool available() const { return at < data.size(); }
  size_t read(uint8_t* out, size_t wanted) {
    const size_t n = std::min(data.size() - at, wanted);
    memcpy(out, data.data() + at, n); at += n; return n;
  }
};
namespace mime {
  enum { txt };
  struct Entry { const char* mimeType; };
  Entry mimeTable[] = {{"text/plain"}};
}
static const char* filename = "filename";
static const char* Content_Type = "Content-Type";
enum { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };
struct HTTPUpload {
  int status;
  String name, filename, type;
  size_t totalSize = 0, currentSize = 0;
  uint8_t buf[HTTP_UPLOAD_BUFLEN]{};
};
struct UploadHandler {
  std::string data;
  template<class T> bool canUpload(T&, int) { return true; }
  template<class T> void upload(T&, int, HTTPUpload& upload) {
    if (upload.status == UPLOAD_FILE_WRITE)
      data.append(reinterpret_cast<const char*>(upload.buf), upload.currentSize);
  }
};
struct RequestArgument { String key, value; };
enum class LineStatus { Ok, TooLong, TimedOut };
@LINE@
class WebServer {
 public:
  NetworkClient _currentClient;
  bool _chunked = false;
  std::string header = "HTTP/1.1 200 OK\r\n\r\n";
  explicit WebServer(std::shared_ptr<Peer> peer) : _currentClient(std::move(peer)) {}
  RequestArgument* _postArgs = nullptr;
  RequestArgument* _currentArgs = nullptr;
  int _postArgsLen = 0, _currentArgCount = 0, _currentUri = 0;
  std::unique_ptr<HTTPUpload> _currentUpload;
  UploadHandler* _currentHandler = nullptr;
  ~WebServer() { delete[] _postArgs; delete[] _currentArgs; }
  bool _parseForm(NetworkClient&, const String&, uint32_t);
  bool _parseFormUploadAborted();
  int _uploadReadByte(NetworkClient&);
  void _uploadWriteByte(uint8_t);
  bool hasArg(const char*) { return false; }
  String arg(const char*) { return ""; }
  void setContentLength(size_t) {}
  void _prepareHeader(String& out, int, const char*, size_t) { out = header; }
  NetworkClient client() { return _currentClient; }
  @CLIENT_WRITE@
  void send(int, const char*, const String&);
  void send(int, const char*, const char*);
  void sendContent(const String&);
  void sendContent(const char*, size_t);
};
@UPLOAD_READ@
@UPLOAD_WRITE@
@UPLOAD_ABORT@
@FORM@
@SEND_STRING@
@SEND_CHARS@
@SEND_CONTENT@
@SEND_CONTENT_STRING@
void sendStaticCacheHeaders(WebServer&, bool, bool, const String&) {}
@STATIC@
@EMBEDDED_STATIC@
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t*, size_t) = 0;
};
namespace json_stream_detail { @JSON_BUFFER@; }
void sendJsonStream(WebServer& server, const JsonDocument& doc, int code = 200) { @JSON@ }
void sendSerializedJson(WebServer& server, const char* data, size_t len, int code = 200) { @CACHED@ }
static std::shared_ptr<Peer> beginCase(const char* name, uint32_t initial = 100, bool taskEnrolled = true) {
  ++cases; currentCase = name; clockMs = initial;
  elapsedMs = lastFeedMs = maxGapMs = progressBytes = fedBytes = feeds = 0;
  enrolled = taskEnrolled;
  return std::make_shared<Peer>();
}
static void checkProgress(bool expectLong = true) {
  check(!expectLong || elapsedMs > watchdogWindow, "fixture must exceed one watchdog window");
  check(feeds > 0, "transferred bytes must feed the enrolled task");
  check(maxGapMs < watchdogWindow, "transfer held task beyond the watchdog window");
}
static void checkBounded(bool expectFeed = false) {
  check(elapsedMs < watchdogWindow, "idle/disconnected transfer exceeded watchdog window");
  check(maxGapMs < watchdogWindow, "idle wait exceeded watchdog window");
  check(expectFeed ? feeds > 0 : feeds == 0, "idle path must not invent progress feeds");
}
int main() {
  for (bool plain : {false, true}) {
    for (uint32_t initial : {100u, UINT32_MAX - 500u}) {
      auto peer = beginCase(plain ? "plain progressing ingress" : "raw progressing ingress", initial);
      peer->incoming = R"({"_type":"v1simple_backup","brightness":77})";
      peer->incoming.resize(128 * 1024, ' ');
      NetworkClient client(peer);
      if (plain) {
        size_t length = 0;
        char* body = readBytesWithTimeout(client, peer->incoming.size(), length, 5000);
        check(body && std::string(body, length) == peer->incoming, "plain body must remain exact");
        free(body);
      } else {
        Receiver receiver;
        check(receiver.receive(client, peer->incoming.size()), "raw body must complete");
        check(receiver.handler.body == peer->incoming, "raw body must remain exact");
      }
      checkProgress();
    }
    for (size_t partial : {size_t(0), size_t(512)}) {
      for (bool disconnect : {false, true}) {
        auto peer = beginCase(plain ? "plain idle/disconnected ingress" : "raw idle/disconnected ingress");
        peer->incoming.assign(8192, 'b');
        peer->readLimit = partial;
        peer->disconnectAtLimit = disconnect;
        NetworkClient client(peer);
        if (plain) {
          size_t length = 0;
          char* body = readBytesWithTimeout(client, peer->incoming.size(), length, 5000);
          check(length == partial, "plain incomplete body must preserve actual length");
          free(body);
        } else {
          Receiver receiver;
          check(!receiver.receive(client, peer->incoming.size()), "raw incomplete body must abort");
        }
        checkBounded(partial != 0);
      }
    }
  }
  for (int owner = 0; owner < 5; ++owner) {
    for (uint32_t initial : {100u, UINT32_MAX - 500u}) {
      const char* names[] = {"static output", "streamed JSON output", "cached JSON output", "header output",
                             "embedded static output"};
      auto peer = beginCase(names[owner], initial);
      WebServer server(peer);
      std::string expected;
      if (owner == 0) {
        File file{std::string(32768, 'x')};
        expected = file.data;
        check(streamOpenFile(server, file, "/asset.js", "application/javascript", file.data.size(),
                             true, true, "etag"), "static stream must complete");
      } else if (owner == 1 || owner == 2) {
        JsonDocument doc;
        doc["value"] = std::string(32768, 'x');
        serializeJson(doc, expected);
        if (owner == 1) sendJsonStream(server, doc);
        else sendSerializedJson(server, expected.data(), expected.size());
      } else if (owner == 3) {
        server.header.assign(8192, 'H');
        expected = "ok";
        server.send(200, "text/plain", expected.c_str());
      } else {
        expected.assign(32768, 'e');
        const EmbeddedWebAssets::Asset asset{"/index.html",
            reinterpret_cast<const uint8_t*>(expected.data()), expected.size()};
        check(streamEmbeddedAsset(server, asset, asset.path, "text/html", false, false, "etag"),
              "embedded static stream must complete");
      }
      check(peer->outgoing == server.header + expected, "output header/body must remain exact");
      checkProgress();
    }
  }
  for (uint32_t initial : {100u, UINT32_MAX - 500u}) {
    auto peer = beginCase("one write with many successful partial sends", initial);
    NetworkClient client(peer);
    const std::string data(32768, 'w');
    check(client.write(data.data(), data.size()) == data.size(), "one write must complete");
    check(peer->outgoing == data, "one write must preserve every byte");
    checkProgress();
  }
  for (size_t partial : {size_t(0), size_t(128)}) {
    for (bool disconnect : {false, true}) {
      for (int stall = 0; stall < 3; ++stall) {
        auto peer = beginCase("idle/partial/disconnected output", UINT32_MAX - 500u);
        peer->writeLimit = partial;
        peer->disconnectAtLimit = disconnect;
        peer->writeEagain = stall == 1;
        peer->writeZero = stall == 2;
        NetworkClient client(peer);
        const std::string data(8192, 'w');
        check(!client_write_retry::writeAll(client, reinterpret_cast<const uint8_t*>(data.data()), data.size()),
              "incomplete output must fail");
        check(peer->writeAt == partial, "short send must retain actual progress");
        checkBounded(partial != 0);
      }
    }
  }
  {
    auto peer = beginCase("select error retains partial count and closes socket");
    peer->writeLimit = 128;
    peer->disconnectAtLimit = true;
    NetworkClient client(peer);
    const std::string data(8192, 'e');
    check(client.write(data.data(), data.size()) == 128, "select error must report bytes already sent");
    check(!peer->connected, "select error must terminate the socket");
    checkBounded(true);
  }
  for (int owner = 0; owner < 5; ++owner) {
    for (size_t partial : {size_t(0), size_t(128)}) {
      const char* names[] = {"static output stall", "streamed JSON stall", "cached JSON stall", "header stall",
                             "embedded static stall"};
      auto peer = beginCase(names[owner]);
      peer->writeLimit = partial;
      WebServer server(peer);
      if (owner == 0) {
        File file{std::string(8192, 'x')};
        check(!streamOpenFile(server, file, "/asset.js", "application/javascript", file.data.size(),
                              false, true, "etag"), "static stall must fail");
      } else if (owner == 1 || owner == 2) {
        JsonDocument doc;
        doc["value"] = std::string(8192, 'x');
        std::string body;
        serializeJson(doc, body);
        if (owner == 1) sendJsonStream(server, doc);
        else sendSerializedJson(server, body.data(), body.size());
      } else if (owner == 3) {
        server.header.assign(8192, 'H');
        server.send(200, "text/plain", "body");
      } else {
        const std::string body(8192, 'e');
        const EmbeddedWebAssets::Asset asset{"/index.html",
            reinterpret_cast<const uint8_t*>(body.data()), body.size()};
        check(!streamEmbeddedAsset(server, asset, asset.path, "text/html", false, false, "etag"),
              "embedded static stall must fail");
      }
      check(peer->writeAt == partial, "output stall must not duplicate or invent bytes");
      check(!peer->connected, "output stall must terminate the socket");
      checkBounded(partial != 0);
    }
  }
  for (bool read : {false, true}) {
    auto peer = beginCase("unenrolled task transfer", 100, false);
    NetworkClient client(peer);
    const std::string data(8192, 'u');
    if (read) {
      peer->incoming = data;
      std::vector<char> out(data.size());
      check(client.readBytes(out.data(), out.size()) == out.size(), "unenrolled read must complete");
    } else {
      check(client.write(data.data(), data.size()) == data.size(), "unenrolled write must complete");
    }
    check(progressBytes == data.size(), "unenrolled transfer must still make real progress");
    check(feeds == 0, "unenrolled tasks must never reset the watchdog");
  }
  {
    auto peer = beginCase("partial plain body stall followed by error response");
    peer->incoming.assign(8192, 'p');
    peer->readLimit = 512;
    peer->writeLimit = 0;
    NetworkClient client(peer);
    size_t length = 0;
    char* body = readBytesWithTimeout(client, peer->incoming.size(), length, 5000);
    check(length == 512, "stalled plain body must preserve actual partial length");
    free(body);
    WebServer server(peer);
    server.send(400, "text/plain", "incomplete body");
    check(!peer->connected, "stalled plain body must close before attempted error response");
    checkBounded(true);
  }
  const std::string form =
      "--b\r\nContent-Disposition: form-data; name=\"address\"\r\n\r\nAA:BB:CC:DD:EE:FF\r\n"
      "--b\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\nAdapter\r\n--b--\r\n";
  for (uint32_t interval : {1u, 50u}) {
    for (uint32_t initial : {100u, UINT32_MAX - 500u}) {
      auto peer = beginCase("legacy multipart progressing fields", initial);
      peer->readChunk = 1;
      peer->readInterval = interval;
      peer->incoming = form;
      const std::string headers =
          "POST /api/obd/devices/name HTTP/1.1\r\nX-V1Simple-Request: maintenance-ui\r\n"
          "Content-Type: multipart/form-data; boundary=b\r\nContent-Length: " +
          std::to_string(form.size()) + "\r\n\r\n";
      check(WifiMaintenanceHttpPreflight::evaluate(headers.data(), headers.size(), true) ==
                WifiMaintenanceHttpPreflight::Decision::AllowBodyParsing,
            "legacy route must reach the real multipart reader");
      WebServer server(peer);
      check(server._parseForm(server._currentClient, "b", form.size()), "multipart fields must parse");
      check(server._currentArgCount == 2, "multipart must publish both fields");
      if (server._currentArgCount == 2) {
        check(server._currentArgs[0].key == "address" &&
              server._currentArgs[0].value == "AA:BB:CC:DD:EE:FF", "first multipart field must remain exact");
        check(server._currentArgs[1].key == "name" &&
              server._currentArgs[1].value == "Adapter", "second multipart field must remain exact");
      }
      check(feeds == form.size(), "each consumed single byte must feed exactly once");
      checkProgress(interval == 50);
    }
  }
  const size_t fieldEndingCr = form.find("\r\n--b", 6) + 1;
  for (size_t partial : {size_t(0), size_t(2), size_t(4), size_t(50), fieldEndingCr, form.size() - 1}) {
    for (bool disconnect : {false, true}) {
      for (uint32_t initial : {100u, UINT32_MAX - 30u}) {
        auto peer = beginCase("multipart partial/stalled/disconnected or missing LF", initial);
        peer->readChunk = 1;
        peer->readInterval = 1;
        peer->incoming = form;
        peer->readLimit = partial;
        peer->disconnectAtLimit = disconnect;
        peer->writeLimit = 0;
        WebServer server(peer);
        check(!server._parseForm(server._currentClient, "b", form.size()), "incomplete multipart must abort");
        check(server._currentArgCount == 0, "aborted multipart must not publish partial request fields");
        check(!peer->connected, "timed out multipart must close before an error response");
        server.send(400, "text/plain", "incomplete multipart");
        checkBounded(partial != 0);
      }
    }
  }
  const std::string uploadPrefix =
      "--b\r\nContent-Disposition: form-data; name=\"blob\"; filename=\"fixture.txt\"\r\n"
      "Content-Type: text/plain\r\n\r\n";
  const std::string fileContents(128, 'f');
  const std::string upload = uploadPrefix + fileContents + "\r\n--b--\r\n";
  for (uint32_t interval : {1u, 50u}) {
    for (uint32_t initial : {100u, UINT32_MAX - 500u}) {
      auto peer = beginCase("multipart upload byte-reader progress", initial);
      peer->incoming = upload;
      peer->readChunk = 1;
      peer->readInterval = interval;
      WebServer server(peer);
      UploadHandler handler;
      server._currentHandler = &handler;
      check(server._parseForm(server._currentClient, "b", upload.size()), "multipart file must parse");
      check(handler.data == fileContents, "multipart upload bytes must remain exact");
      checkProgress(interval == 50);
    }
  }
  for (size_t partial : {uploadPrefix.size(), uploadPrefix.size() + 10, upload.size() - 1}) {
    for (bool disconnect : {false, true}) {
      auto peer = beginCase("multipart upload byte-reader stall or disconnect", UINT32_MAX - 100u);
      peer->incoming = upload;
      peer->readChunk = 1;
      peer->readInterval = 1;
      peer->readLimit = partial;
      peer->disconnectAtLimit = disconnect;
      peer->writeLimit = 0;
      WebServer server(peer);
      UploadHandler handler;
      server._currentHandler = &handler;
      check(!server._parseForm(server._currentClient, "b", upload.size()), "incomplete multipart file must abort");
      check(!peer->connected, "stalled multipart file must close before an error response");
      server.send(400, "text/plain", "incomplete file");
      checkBounded(true);
    }
  }
  for (size_t length : {size_t{1}, size_t{69}, size_t{70}, size_t{71}}) {
    auto peer = beginCase("multipart boundary capacity");
    const std::string boundary(length, 'b');
    // Include a partial delimiter match so retained bytes must be flushed
    // correctly before the real closing delimiter.
    const std::string contents = "start\r\n--" + boundary.substr(0, length - 1) + "Xend";
    peer->incoming = "--" + boundary +
        "\r\nContent-Disposition: form-data; name=\"blob\"; filename=\"fixture.txt\"\r\n"
        "Content-Type: text/plain\r\n\r\n" + contents + "\r\n--" + boundary + "--\r\n";
    WebServer server(peer);
    UploadHandler handler;
    server._currentHandler = &handler;
    const bool parsed = server._parseForm(server._currentClient, boundary.c_str(), peer->incoming.size());
    if (length <= 70) {
      check(parsed, "supported boundary must parse");
      check(handler.data == contents, "boundary buffer must preserve exact upload bytes");
    } else {
      check(!parsed, "oversize boundary must reject before using the buffer");
      check(!server._currentUpload, "oversize boundary must not begin an upload");
      check(handler.data.empty(), "rejected boundary must not publish file bytes");
    }
  }
  for (uint32_t initial : {100u, UINT32_MAX - 500u}) {
    auto peer = beginCase("direct buffered receive progress", initial);
    peer->incoming.assign(8192, 'r');
    peer->readChunk = 128;
    peer->readInterval = 100;
    NetworkClient client(peer);
    std::string received;
    while (received.size() < peer->incoming.size()) {
      uint8_t chunk[128];
      const int count = client.read(chunk, sizeof(chunk));
      check(count >= 0, "direct buffered receive must stay connected");
      if (count > 0) received.append(reinterpret_cast<const char*>(chunk), count);
      else delay(1);
    }
    check(received == peer->incoming, "direct buffered receive must preserve bytes");
    check(feeds == 64, "buffered owner must feed exactly once per positive read");
    checkProgress();
  }
  printf("HTTP transfer lifecycle: %u cases, %u failures\n", cases, failures);
  return failures ? 1 : 0;
}
"""


STATIC_FILESYSTEM = r"""
#pragma once
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
struct File {
  std::string data;
  bool present = false;
  size_t at = 0;
  explicit operator bool() const { return present; }
  size_t size() const { return data.size(); }
  bool available() const { return at < data.size(); }
  size_t read(uint8_t* out, size_t wanted) {
    const size_t n = std::min(data.size() - at, wanted);
    memcpy(out, data.data() + at, n); at += n; return n;
  }
  void close() {}
};
struct LittleFSClass {
  size_t accesses = 0;
  std::map<std::string, std::string> files;
  bool exists(const char* path) { ++accesses; return files.count(path) != 0; }
  File open(const char* path, const char*) {
    ++accesses;
    const auto found = files.find(path);
    return found == files.end() ? File{} : File{found->second, true};
  }
};
inline LittleFSClass LittleFS;
"""


STATIC_DISPATCH = r"""
#include <cstdio>
#include <string>
#include "wifi_manager_internals.h"
#include "LittleFS.h"
#include "modules/wifi/embedded_web_assets.h"
SerialClass Serial;
unsigned long mockMillis = 0, mockMicros = 0;
static unsigned failures = 0, cases = 0;
static void check(bool ok, const char* reason) {
  ++cases;
  if (!ok) { ++failures; fprintf(stderr, "FAIL embedded dispatch: %s\n", reason); }
}
static std::string bytes(const char* path) {
  const auto* asset = EmbeddedWebAssets::find(path);
  return asset ? std::string(reinterpret_cast<const char*>(asset->data), asset->size) : std::string();
}
static std::string body(const WebServer& server) {
  return std::string(server.lastBody.c_str(), server.lastBody.length());
}
static bool serve(WebServer& server, const char* path, const char* type = "application/javascript") {
  const size_t before = LittleFS.accesses;
  const bool result = serveLittleFSFileHelper(server, path, type);
  check(LittleFS.accesses == before, "managed or rejected URL must never access LittleFS");
  return result;
}
int main() {
  LittleFS.files = {{"/index.html", "old HTML"}, {"/_app/env.js", "old environment"},
                   {"/_app/immutable/current.js", "old raw JavaScript"},
                   {"/_app/immutable/current.js.gz", "old compressed JavaScript"},
                   {"/_app/immutable/old.js.gz", "stale chunk"},
                   {"/branding/mark.png", "old brand"}, {"/branding/old.png", "stale brand"},
                   {"/audio/alert.mul", "saved audio"}};
  for (const char* encoding : {"", "gzip"}) {
    WebServer server;
    server.setHeader("Accept-Encoding", encoding);
    check(serve(server, "/index.html", "text/html"), "HTML must be available with or without gzip");
    check(body(server) == bytes("/index.html"), "current firmware HTML must override old filesystem HTML");
    check(server.lastContentType == "text/html", "HTML MIME type must remain exact");
    check(server.lastContentLength == bytes("/index.html").size(), "HTML length must remain exact");
    check(server.sentHeader("Content-Encoding") == "", "raw HTML must not claim gzip");
    check(server.sentHeader("Cache-Control") == "no-store, no-cache, must-revalidate", "HTML must not be cached");
    check(server.sentHeader("Vary") == "Accept-Encoding", "static representation must vary on encoding");
  }
  for (const char* encoding : {"gzip", "br, gzip", "GZip", "gzip;q=0.5", "*;q=1"}) {
    WebServer server;
    server.setHeader("Accept-Encoding", encoding);
    check(serve(server, "/_app/immutable/current.js"), "accepted gzip asset must be served");
    check(body(server) == bytes("/_app/immutable/current.js.gz"), "gzip bytes must match the firmware");
    check(server.sentHeader("Content-Encoding") == "gzip", "gzip response must identify its encoding");
    check(server.lastContentType == "application/javascript", "gzip response retains requested MIME type");
    check(server.sentHeader("Cache-Control") == "public, max-age=31536000, immutable", "hashed assets retain immutable caching");
    check(server.sentHeader("ETag").indexOf(EmbeddedWebAssets::fingerprint()) >= 0, "ETag must bind the embedded bundle identity");
    WebServer cached;
    cached.setHeader("Accept-Encoding", encoding);
    cached.setHeader("If-None-Match", server.sentHeader("ETag"));
    check(serve(cached, "/_app/immutable/current.js"), "matching immutable request must be handled");
    check(cached.lastStatusCode == 304 && cached.lastBody.length() == 0, "matching ETag must omit the body");
    check(cached.sentHeader("ETag") == server.sentHeader("ETag"), "304 must retain representation ETag");
  }
  for (const char* encoding : {"", "br", "gzip;q=0", "gzip;q=0.000, *;q=1", "notgzip", "gzip;q=invalid"}) {
    WebServer server;
    server.setHeader("Accept-Encoding", encoding);
    check(!serve(server, "/_app/immutable/current.js"), "unaccepted gzip-only asset must not revive a filesystem twin");
    check(server.sendCount == 0, "missing acceptable representation leaves the caller to return 404");
    check(serve(server, "/_app/env.js"), "raw environment asset remains available");
    check(body(server) == bytes("/_app/env.js"), "raw response must match current firmware");
    check(server.sentHeader("Content-Encoding") == "", "raw environment must not claim gzip");
  }
  {
    WebServer server;
    server.setHeader("Accept-Encoding", "gzip");
    check(serve(server, "/_app/env.js"), "environment gzip twin must be available");
    check(body(server) == bytes("/_app/env.js.gz"), "gzip must be preferred when both representations exist");
    check(server.sentHeader("Cache-Control") == "no-store, no-cache, must-revalidate", "environment must not receive immutable cache policy");
  }
  for (const char* path : {"/_app/immutable/old.js", "/branding/old.png", "/settings.html"}) {
    WebServer server;
    server.setHeader("Accept-Encoding", "gzip");
    check(!serve(server, path), "missing managed asset must not fall through to storage");
  }
  for (const char* path : {static_cast<const char*>(nullptr), "/v1profiles/private.json", "/_app/immutable/../../v1profiles/private.json",
                           "/branding/../settings.json", "/audio/../settings.json"}) {
    WebServer server;
    check(!serve(server, path), "unsafe or private path must be rejected without filesystem access");
  }
  for (const char* path : {"/branding/mark.png", "/_app/immutable/font.woff2"}) {
    WebServer server;
    check(serve(server, path, "application/octet-stream"), "binary embedded asset must be served");
    check(body(server) == bytes(path), "binary assets must retain embedded NUL bytes");
  }
  {
    WebServer server;
    server.setClientMaxWriteSize(7);
    check(serve(server, "/index.html", "text/html"), "short writes must finish the embedded response");
    check(body(server) == bytes("/index.html"), "short writes must not lose or duplicate bytes");
    check(server.clientWriteCalls() > 1, "short-write fixture must exercise repeated writes");
  }
  for (size_t stop : {size_t(0), size_t(513)}) {
    WebServer server;
    server.setClientMaxWriteSize(31);
    server.setClientDisconnectAfterBytes(stop);
    check(serve(server, "/index.html", "text/html"), "disconnected embedded request remains handled");
    check(body(server) == bytes("/index.html").substr(0, stop), "disconnect must retain only actual progress");
    check(server.sendCount == 1, "stream failure must not append an error response after headers");
  }
  {
    WebServer server;
    server.setClientMaxWriteSize(0);
    check(serve(server, "/index.html", "text/html"), "stalled embedded request remains handled");
    check(server.sendCount == 1 && server.lastBody.length() == 0, "stalled writes must stop without a second response");
  }
  {
    WebServer server;
    const size_t before = LittleFS.accesses;
    check(serveLittleFSFileHelper(server, "/audio/alert.mul", "application/octet-stream"), "audio must retain filesystem serving");
    check(LittleFS.accesses > before && body(server) == "saved audio", "filesystem audio must remain unchanged");
  }
  printf("Embedded static dispatch: %u checks, %u failures\n", cases, failures);
  return failures ? 1 : 0;
}
"""


def check_embedded_dispatch(compiler: list[str], out: Path) -> None:
    """Compile the complete production helper and lookup with isolated asset/storage fixtures."""
    fixture = out / "static-dispatch"
    fixture.mkdir()
    (fixture / "wifi_manager_internals.h").write_text(
        '#pragma once\n#include "WebServer.h"\n'
        'bool serveLittleFSFileHelper(WebServer&, const char*, const char*);\n'
    )
    (fixture / "LittleFS.h").write_text(STATIC_FILESYSTEM)
    assets = {
        "/index.html": b"<!doctype html>" + b"current interface " * 200,
        "/_app/env.js": b"window.current=true;",
        "/_app/env.js.gz": gzip.compress(b"window.current=true;", mtime=0),
        "/_app/immutable/current.js.gz": gzip.compress(b"currentJavaScript();" * 100, mtime=0),
        "/_app/immutable/font.woff2": bytes(range(256)) * 2,
        "/branding/mark.png": bytes(range(256)) * 4,
    }
    generated = ['#include "modules/wifi/embedded_web_assets.h"', 'namespace EmbeddedWebAssets {',
                 'static const char kBundleFingerprint[] = "V1SimpleWebBundle:fixture";']
    for i, data in enumerate(assets.values()):
        generated.append(f'static const uint8_t kData{i}[] = {{{",".join(str(b) for b in data)}}};')
    generated.append('static const Asset kAssets[] = {')
    for i, path in enumerate(assets):
        generated.append(f'{{"{path}", kData{i}, sizeof(kData{i})}},')
    generated.append('}; }')
    (fixture / "embedded_web_assets.generated.h").write_text("\n".join(generated) + "\n")
    (fixture / "dispatch.cpp").write_text(STATIC_DISPATCH)
    subprocess.run(compiler + [
        "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DUNIT_TEST", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g",
        "-I" + str(fixture), "-I" + str(ROOT / "test/mocks"),
        "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src"),
        str(fixture / "dispatch.cpp"), str(ROOT / "src/wifi_manager_helpers.cpp"),
        str(ROOT / "src/modules/wifi/embedded_web_assets.cpp"),
        str(ROOT / "src/modules/wifi/wifi_static_path_guard.cpp"),
        "-o", str(fixture / "dispatch"),
    ], check=True)
    subprocess.run([str(fixture / "dispatch")], check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    core = Path(os.environ.get("PIO_CORE_DIR", os.environ.get("PLATFORMIO_CORE_DIR", "~/.platformio"))).expanduser()
    parser.add_argument("--framework-dir", "--framework", dest="framework", type=Path,
                        default=core / "packages/framework-arduinoespressif32")
    parser.add_argument("--baseline", action="store_true",
                        help="exercise original network/idle behavior; expected to fail regression checks")
    args = parser.parse_args()
    replacements = production_sources(args.framework, args.baseline)
    compiler = shlex.split(os.environ["CXX"]) if os.environ.get("CXX") else [
        shutil.which("clang++") or shutil.which("c++") or "c++"
    ]
    source = HARNESS
    for name, value in replacements.items():
        source = source.replace(f"@{name}@", value)
    with tempfile.TemporaryDirectory(prefix="v1-http-lifecycle-") as temporary:
        out = Path(temporary)
        (out / "Arduino.h").write_text(ARDUINO)
        (out / "lifecycle.cpp").write_text(source)
        subprocess.run(compiler + [
            "-std=c++17", "-Werror=vla", "-DUNIT_TEST", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g",
            "-I" + str(out), "-I" + str(ROOT / "include"),
            "-I" + str(ROOT / "src"),
            "-I" + str(ROOT / ".pio/libdeps/native-sanitized/ArduinoJson/src"),
            str(out / "lifecycle.cpp"), "-o", str(out / "lifecycle"),
        ], check=True)
        subprocess.run([str(out / "lifecycle")], check=True)
        check_embedded_dispatch(compiler, out)


if __name__ == "__main__":
    main()
