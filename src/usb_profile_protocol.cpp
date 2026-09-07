#include "usb_profile_protocol.h"

#include <cstdio>
#include <cstring>

namespace {
constexpr char Prefix[] = "@V1USB1 ";

bool number(const char* text, uint32_t& value, unsigned base = 10) {
    if (!text || !*text) return false;
    uint32_t result = 0;
    for (const char* p = text; *p; ++p) {
        unsigned digit = *p >= '0' && *p <= '9' ? static_cast<unsigned>(*p - '0') :
                         *p >= 'a' && *p <= 'f' ? static_cast<unsigned>(*p - 'a' + 10) :
                         *p >= 'A' && *p <= 'F' ? static_cast<unsigned>(*p - 'A' + 10) : 99;
        if (digit >= base || result > (UINT32_MAX - digit) / base) return false;
        result = result * base + digit;
    }
    value = result;
    return true;
}

void escapedDetail(const char* text, char* output, size_t size) {
    size_t n = 0;
    if (text) {
        for (size_t i = 0; text[i] && i < 64 && n + 3 < size; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c < 32 || c >= 127) { output[n++] = '?'; continue; }
            if (c == '"' || c == '\\') output[n++] = '\\';
            output[n++] = static_cast<char>(c);
        }
    }
    output[n] = 0;
}
}  // namespace

UsbProfileProtocol::UsbProfileProtocol(UsbProfileTransport& transport, UsbProfileBackend& backend)
    : transport_(transport), backend_(backend) {}

UsbProfileProtocol::~UsbProfileProtocol() { clearTransfer(); }

uint32_t UsbProfileProtocol::crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xffffffffu;
}

void UsbProfileProtocol::clearTransfer() {
    if (document_) backend_.release(document_);
    document_ = nullptr;
    documentLength_ = received_ = 0;
    expectedCrc_ = 0;
    receiving_ = false;
}

void UsbProfileProtocol::reply(uint32_t id, const char* json) {
    const int size = std::snprintf(output_, sizeof(output_), "\n%s%lu %s\n", Prefix,
                                   static_cast<unsigned long>(id), json);
    if (size <= 0 || static_cast<size_t>(size) >= sizeof(output_)) {
        std::snprintf(output_, sizeof(output_), "\n%s%lu {\"ok\":false,\"error\":\"reply_too_large\"}\n",
                      Prefix, static_cast<unsigned long>(id));
    }
    outputLength_ = std::strlen(output_);
    std::memcpy(lastReply_, output_, outputLength_ + 1);
    cachedAction_ = action_;
}

void UsbProfileProtocol::fail(uint32_t id, const char* code, const char* detail) {
    char safe[100];
    escapedDetail(detail, safe, sizeof(safe));
    char json[220];
    std::snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%.40s\",\"detail\":\"%s\"}", code, safe);
    action_ = Action::None;
    reply(id, json);
}

void UsbProfileProtocol::transmit(uint32_t nowMs) {
    if (!outputLength_) return;
    if (static_cast<uint32_t>(nowMs - outputAt_) >= FrameIdleMs) {
        if (action_ != Action::None) fail(lastId_, "transition_expired");
        outputLength_ = 0;
        action_ = Action::None;
        return;
    }
    if (action_ != Action::None && backend_.status().busy) fail(lastId_, "busy");
    if (transport_.availableForWrite() < static_cast<int>(outputLength_)) return;
    // HWCDC can return short despite availableForWrite(). Restart the whole
    // tagged frame on the next tick: its leading newline discards a torn reply.
    // The host matches IDs and checks the complete document CRC before use.
    if (transport_.write(reinterpret_cast<const uint8_t*>(output_), outputLength_) != outputLength_) return;
    outputLength_ = 0;
    const Action action = action_;
    action_ = Action::None;
    cachedAction_ = Action::None;
    if (action != Action::None) {
        const auto state = backend_.status();
        if (state.busy) return;
        if (action == Action::Maintenance && !state.maintenance) backend_.enterMaintenance();
        if (action == Action::Normal && state.maintenance) backend_.restartNormal();
    }
}

void UsbProfileProtocol::tick(uint32_t nowMs) {
    if (document_ && static_cast<uint32_t>(nowMs - transferAt_) >= SessionIdleMs) clearTransfer();
    if (outputLength_) { transmit(nowMs); return; }
    if (inputLength_ && static_cast<uint32_t>(nowMs - inputAt_) >= FrameIdleMs) {
        inputLength_ = 0;
        discarding_ = true;
    }
    for (size_t read = 0; read < RxBytesPerTick && transport_.available() > 0; ++read) {
        const int value = transport_.read();
        if (value < 0) break;
        inputAt_ = nowMs;
        const char c = static_cast<char>(value);
        if (c == '\n') {
            if (!discarding_ && inputLength_) {
                input_[inputLength_] = 0;
                dispatch(nowMs);
            }
            inputLength_ = 0;
            discarding_ = false;
            break; // At most one request per main-loop tick.
        }
        if (discarding_) continue;
        if (c < 32 || c > 126 || inputLength_ + 1 >= sizeof(input_)) {
            discarding_ = true;
            inputLength_ = 0;
            continue;
        }
        input_[inputLength_++] = c;
    }
    if (outputLength_) { outputAt_ = nowMs; transmit(nowMs); }
}

void UsbProfileProtocol::dispatch(uint32_t nowMs) {
    if (std::strncmp(input_, Prefix, sizeof(Prefix) - 1) != 0) return;
    char request[FrameBytes];
    std::memcpy(request, input_, inputLength_ + 1);
    char* cursor = input_ + sizeof(Prefix) - 1;
    char* space = std::strchr(cursor, ' ');
    if (!space) return;
    *space = 0;
    uint32_t id = 0;
    if (!number(cursor, id) || id == 0) return;
    if (id == lastId_) {
        if (std::strcmp(request, lastRequest_) != 0) {
            char savedReply[FrameBytes];
            std::strcpy(savedReply, lastReply_);
            const Action savedAction = cachedAction_;
            fail(id, "request_id_conflict");
            std::strcpy(lastReply_, savedReply);
            cachedAction_ = savedAction;
            return;
        }
        std::strcpy(output_, lastReply_);
        outputLength_ = std::strlen(output_);
        action_ = cachedAction_;
        transferAt_ = nowMs;
        backend_.activity(nowMs);
        return;
    }
    lastId_ = id;
    std::strcpy(lastRequest_, request);
    action_ = Action::None;
    const char* words[4]{};
    size_t count = 0;
    cursor = space + 1;
    while (*cursor && count < 4) {
        words[count++] = cursor;
        space = std::strchr(cursor, ' ');
        if (!space) break;
        *space = 0;
        cursor = space + 1;
        if (!*cursor) { fail(id, "bad_request"); return; }
    }
    if (!count || count > 3) { fail(id, "bad_request"); return; }
    const auto state = backend_.status();
    char json[220];
    backend_.activity(nowMs);
    transferAt_ = nowMs;
    if (std::strcmp(words[0], "status") == 0 && count == 1) {
        std::snprintf(json, sizeof(json),
            "{\"ok\":true,\"mode\":\"%s\",\"busy\":%s,\"boot\":%lu,\"git\":\"%.7s\",\"image\":\"%.9s\",\"slot\":%u,\"persist\":%u,\"enabled\":%s}",
            state.maintenance ? "maintenance" : "normal", state.busy ? "true" : "false",
            static_cast<unsigned long>(state.boot), state.git, state.image, state.slot, state.persistence,
            state.enabled ? "true" : "false");
        reply(id, json);
        return;
    }
    const bool enter = std::strcmp(words[0], "maintenance") == 0;
    const bool normal = std::strcmp(words[0], "normal") == 0;
    if ((enter || normal) && count == 1) {
        if (enter == state.maintenance) {
            reply(id, state.maintenance ? "{\"ok\":true,\"mode\":\"maintenance\"}" : "{\"ok\":true,\"mode\":\"normal\"}");
        } else if (state.busy) {
            fail(id, "busy");
        } else {
            clearTransfer();
            action_ = enter ? Action::Maintenance : Action::Normal;
            reply(id, "{\"ok\":true,\"mode\":\"restarting\"}");
        }
        return;
    }
    if (!state.maintenance) { fail(id, "maintenance_required"); return; }
    if (state.busy) { fail(id, "busy"); return; }
    if (std::strcmp(words[0], "abort") == 0 && count == 1) {
        clearTransfer(); reply(id, "{\"ok\":true}"); return;
    }
    char error[96]{};
    if (std::strcmp(words[0], "backup") == 0 && count == 1) {
        clearTransfer();
        if (!backend_.backup(document_, documentLength_, error, sizeof(error))) {
            clearTransfer(); fail(id, "backup_failed", error); return;
        }
        if (!document_ || !documentLength_ || documentLength_ > MaxDocumentBytes) {
            clearTransfer(); fail(id, "document_too_large"); return;
        }
        std::snprintf(json, sizeof(json), "{\"ok\":true,\"bytes\":%lu,\"crc32\":\"%08lx\"}",
                      static_cast<unsigned long>(documentLength_),
                      static_cast<unsigned long>(crc32(document_, documentLength_)));
        reply(id, json); return;
    }
    uint32_t offset = 0;
    if (std::strcmp(words[0], "read") == 0 && count == 2 && number(words[1], offset)) {
        if (!document_ || receiving_ || offset >= documentLength_) { fail(id, "bad_offset"); return; }
        const size_t length = documentLength_ - offset < ChunkBytes ? documentLength_ - offset : ChunkBytes;
        char hex[ChunkBytes * 2 + 1];
        constexpr char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < length; ++i) {
            hex[2 * i] = digits[document_[offset + i] >> 4];
            hex[2 * i + 1] = digits[document_[offset + i] & 15];
        }
        hex[2 * length] = 0;
        std::snprintf(json, sizeof(json), "{\"ok\":true,\"offset\":%lu,\"data\":\"%s\"}",
                      static_cast<unsigned long>(offset), hex);
        reply(id, json); return;
    }
    uint32_t crc = 0;
    if (std::strcmp(words[0], "begin") == 0 && count == 3 && number(words[1], offset) &&
        std::strlen(words[2]) == 8 && number(words[2], crc, 16)) {
        if (offset == 0 || offset > MaxDocumentBytes) { fail(id, "document_too_large"); return; }
        clearTransfer();
        document_ = backend_.allocate(static_cast<size_t>(offset) + 1);
        if (!document_) { fail(id, "out_of_memory"); return; }
        documentLength_ = offset;
        expectedCrc_ = crc;
        receiving_ = true;
        reply(id, "{\"ok\":true,\"offset\":0}"); return;
    }
    if (std::strcmp(words[0], "write") == 0 && count == 3 && number(words[1], offset)) {
        const size_t n = std::strlen(words[2]);
        if (!document_ || !receiving_ || offset != received_ || n == 0 || n % 2 != 0 ||
            n > ChunkBytes * 2 || n / 2 > documentLength_ - received_) { fail(id, "bad_chunk"); return; }
        uint8_t chunk[ChunkBytes]{};
        for (size_t i = 0; i < n / 2; ++i) {
            char pair[3] = {words[2][2 * i], words[2][2 * i + 1], 0};
            uint32_t value = 0;
            if (!number(pair, value, 16)) { fail(id, "bad_chunk"); return; }
            chunk[i] = static_cast<uint8_t>(value);
        }
        std::memcpy(document_ + received_, chunk, n / 2);
        received_ += n / 2;
        std::snprintf(json, sizeof(json), "{\"ok\":true,\"offset\":%lu}", static_cast<unsigned long>(received_));
        reply(id, json); return;
    }
    if (std::strcmp(words[0], "commit") == 0 && count == 1) {
        if (!document_ || !receiving_ || received_ != documentLength_) { fail(id, "incomplete_document"); return; }
        if (crc32(document_, documentLength_) != expectedCrc_) {
            clearTransfer(); fail(id, "checksum_mismatch"); return;
        }
        document_[documentLength_] = 0;
        bool pending = false;
        int profiles = 0;
        const bool ok = backend_.apply(document_, documentLength_, pending, profiles, error, sizeof(error));
        clearTransfer(); // Never reapply a committed or failed document on a later commit.
        if (!ok) { fail(id, "apply_failed", error); return; }
        std::snprintf(json, sizeof(json), "{\"ok\":true,\"stored\":true,\"backup_pending\":%s,\"profiles\":%d}",
                      pending ? "true" : "false", profiles);
        reply(id, json); return;
    }
    fail(id, "bad_request");
}
