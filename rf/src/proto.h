// Wire format shared by the radio daemon (zrfd) and its clients.
// See rf/README.md for the protocol.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

namespace zrf {

// Spectrum levels travel as one byte per column: 0.5 dB steps from -120 dB.
inline uint8_t db_to_byte(float db) {
    return uint8_t(std::clamp(std::lround((db + 120.0f) * 2.0f), 0L, 255L));
}
inline float byte_to_db(uint8_t b) { return b * 0.5f - 120.0f; }

inline std::string to_hex(const std::vector<uint8_t>& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string s(bytes.size() * 2, '0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        s[2 * i] = digits[bytes[i] >> 4];
        s[2 * i + 1] = digits[bytes[i] & 15];
    }
    return s;
}

inline bool from_hex(const std::string& s, std::vector<uint8_t>& out) {
    auto val = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    if (s.size() % 2) return false;
    out.resize(s.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        int hi = val(s[2 * i]), lo = val(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = uint8_t(hi << 4 | lo);
    }
    return true;
}

inline std::string default_socket_path() {
    if (const char* p = std::getenv("ZRFD_SOCKET")) return p;
    if (const char* d = std::getenv("XDG_RUNTIME_DIR")) return std::string(d) + "/zrfd.sock";
    return "/tmp/zrfd-" + std::to_string(getuid()) + ".sock";
}

}  // namespace zrf
