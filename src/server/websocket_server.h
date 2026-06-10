#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <optional>

#include <unistd.h>
#include <sys/socket.h>

#include "core/common/logging.h"

namespace illuminator {

static const char* kWebSocketGUID = "258EAFA5-E914-47DA-95CA-5AB5DC11D65B";

namespace detail {

// Inline SHA-1 implementation (RFC 3174) — replaces OpenSSL dependency
class Sha1 {
public:
    void Update(const void* data, size_t len) {
        auto* p = static_cast<const uint8_t*>(data);
        while (len > 0) {
            size_t room = 64 - buf_len_;
            size_t take = (len < room) ? len : room;
            std::memcpy(buf_ + buf_len_, p, take);
            buf_len_ += take; p += take; len -= take; total_ += take;
            if (buf_len_ == 64) { ProcessBlock(buf_); buf_len_ = 0; }
        }
    }

    void Final(uint8_t digest[20]) {
        uint64_t bits = total_ * 8;
        uint8_t pad = 0x80;
        Update(&pad, 1);
        pad = 0;
        while (buf_len_ != 56) Update(&pad, 1);
        uint8_t len_be[8];
        for (int i = 7; i >= 0; --i) { len_be[i] = bits & 0xFF; bits >>= 8; }
        Update(len_be, 8);
        for (int i = 0; i < 5; ++i) {
            digest[i*4+0] = (h_[i] >> 24) & 0xFF;
            digest[i*4+1] = (h_[i] >> 16) & 0xFF;
            digest[i*4+2] = (h_[i] >> 8)  & 0xFF;
            digest[i*4+3] = h_[i] & 0xFF;
        }
    }

private:
    static uint32_t RotL(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void ProcessBlock(const uint8_t block[64]) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(block[i*4]) << 24) | (uint32_t(block[i*4+1]) << 16)
                 | (uint32_t(block[i*4+2]) << 8) | block[i*4+3];
        for (int i = 16; i < 80; ++i)
            w[i] = RotL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d);       k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t tmp = RotL(a, 5) + f + e + k + w[i];
            e = d; d = c; c = RotL(b, 30); b = a; a = tmp;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
    }

    uint32_t h_[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t buf_[64] = {};
    size_t buf_len_ = 0;
    uint64_t total_ = 0;
};

inline std::string Base64Encode(const unsigned char* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(4 * ((len + 2) / 3));
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? tbl[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? tbl[n & 0x3F] : '=';
    }
    return out;
}

inline std::string ComputeAcceptKey(const std::string& client_key) {
    std::string concat = client_key + kWebSocketGUID;
    uint8_t hash[20];
    Sha1 ctx;
    ctx.Update(concat.data(), concat.size());
    ctx.Final(hash);
    return Base64Encode(hash, 20);
}

inline std::string ExtractHeader(const std::string& request,
                                  const std::string& header_name) {
    std::string lower_req = request;
    std::string lower_hdr = header_name;
    for (auto& c : lower_req) c = static_cast<char>(::tolower(c));
    for (auto& c : lower_hdr) c = static_cast<char>(::tolower(c));

    auto pos = lower_req.find(lower_hdr + ":");
    if (pos == std::string::npos) return "";
    pos += lower_hdr.size() + 1;
    while (pos < request.size() && request[pos] == ' ') ++pos;
    auto end = request.find("\r\n", pos);
    if (end == std::string::npos) end = request.size();
    return request.substr(pos, end - pos);
}

inline std::string ExtractPath(const std::string& request) {
    auto sp1 = request.find(' ');
    if (sp1 == std::string::npos) return "/";
    auto sp2 = request.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return "/";
    auto full = request.substr(sp1 + 1, sp2 - sp1 - 1);
    auto q = full.find('?');
    return (q != std::string::npos) ? full.substr(0, q) : full;
}

}  // namespace detail

enum class WsOpcode : uint8_t {
    kContinuation = 0x0,
    kText         = 0x1,
    kBinary       = 0x2,
    kClose        = 0x8,
    kPing         = 0x9,
    kPong         = 0xA,
};

struct WsFrame {
    WsOpcode opcode = WsOpcode::kText;
    bool fin = true;
    std::string payload;
};

class WebSocketCodec {
public:
    static bool IsUpgradeRequest(const std::string& request) {
        auto upgrade = detail::ExtractHeader(request, "Upgrade");
        for (auto& c : upgrade) c = static_cast<char>(::tolower(c));
        return upgrade == "websocket";
    }

    static bool PerformHandshake(int fd, const std::string& request) {
        auto key = detail::ExtractHeader(request, "Sec-WebSocket-Key");
        if (key.empty()) {
            IL_WARN("WebSocket: missing Sec-WebSocket-Key");
            return false;
        }

        auto accept = detail::ComputeAcceptKey(key);
        std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

        ssize_t written = ::write(fd, response.data(), response.size());
        return written == static_cast<ssize_t>(response.size());
    }

    static std::string GetUpgradePath(const std::string& request) {
        return detail::ExtractPath(request);
    }

    static std::vector<uint8_t> EncodeFrame(const std::string& payload,
                                             WsOpcode opcode = WsOpcode::kText) {
        std::vector<uint8_t> frame;
        frame.push_back(0x80 | static_cast<uint8_t>(opcode));

        size_t len = payload.size();
        if (len <= 125) {
            frame.push_back(static_cast<uint8_t>(len));
        } else if (len <= 65535) {
            frame.push_back(126);
            frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(len & 0xFF));
        } else {
            frame.push_back(127);
            for (int i = 7; i >= 0; --i)
                frame.push_back(static_cast<uint8_t>((len >> (8 * i)) & 0xFF));
        }

        frame.insert(frame.end(), payload.begin(), payload.end());
        return frame;
    }

    static std::vector<uint8_t> EncodeCloseFrame(uint16_t code = 1000) {
        std::string payload;
        payload += static_cast<char>((code >> 8) & 0xFF);
        payload += static_cast<char>(code & 0xFF);
        return EncodeFrame(payload, WsOpcode::kClose);
    }

    static std::vector<uint8_t> EncodePongFrame(const std::string& payload) {
        return EncodeFrame(payload, WsOpcode::kPong);
    }

    static std::optional<WsFrame> DecodeFrame(const uint8_t* data, size_t len,
                                               size_t& consumed) {
        consumed = 0;
        if (len < 2) return std::nullopt;

        uint8_t b0 = data[0];
        uint8_t b1 = data[1];

        WsFrame frame;
        frame.fin = (b0 & 0x80) != 0;
        frame.opcode = static_cast<WsOpcode>(b0 & 0x0F);

        bool masked = (b1 & 0x80) != 0;
        uint64_t payload_len = b1 & 0x7F;
        size_t header_size = 2;

        if (payload_len == 126) {
            if (len < 4) return std::nullopt;
            payload_len = (static_cast<uint64_t>(data[2]) << 8) | data[3];
            header_size = 4;
        } else if (payload_len == 127) {
            if (len < 10) return std::nullopt;
            payload_len = 0;
            for (int i = 0; i < 8; ++i)
                payload_len = (payload_len << 8) | data[2 + i];
            header_size = 10;
        }

        if (masked) header_size += 4;
        if (len < header_size + payload_len) return std::nullopt;

        const uint8_t* mask_key = masked ? data + header_size - 4 : nullptr;
        const uint8_t* payload_data = data + header_size;

        frame.payload.resize(payload_len);
        for (uint64_t i = 0; i < payload_len; ++i) {
            frame.payload[i] = masked
                ? static_cast<char>(payload_data[i] ^ mask_key[i % 4])
                : static_cast<char>(payload_data[i]);
        }

        consumed = header_size + payload_len;
        return frame;
    }
};

}  // namespace illuminator
