#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <optional>

#include <openssl/sha.h>
#include <unistd.h>
#include <sys/socket.h>

#include "core/common/logging.h"

namespace illuminator {

static const char* kWebSocketGUID = "258EAFA5-E914-47DA-95CA-5AB5DC11D65B";

namespace detail {

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
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(concat.data()),
         concat.size(), hash);
    return Base64Encode(hash, SHA_DIGEST_LENGTH);
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
