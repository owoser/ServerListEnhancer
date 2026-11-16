#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace sle::server {

inline constexpr std::size_t kMaxLogoDataBytes = 24 * 1024;
inline constexpr std::string_view kLogoDataPrefix = "data:image/png;base64,";

inline bool isValidLogoData(std::string_view data) {
    if (!data.starts_with(kLogoDataPrefix)) {
        return false;
    }
    auto const encoded = data.substr(kLogoDataPrefix.size());
    if (encoded.empty() || encoded.size() % 4 != 0 || encoded.size() > ((kMaxLogoDataBytes + 2) / 3) * 4) {
        return false;
    }
    auto sextet = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<unsigned char> bytes;
    bytes.reserve(encoded.size() / 4 * 3);
    for (std::size_t i = 0; i < encoded.size(); i += 4) {
        bool const last = i + 4 == encoded.size();
        int const a = sextet(encoded[i]);
        int const b = sextet(encoded[i + 1]);
        bool const padC = encoded[i + 2] == '=';
        bool const padD = encoded[i + 3] == '=';
        int const c = padC ? 0 : sextet(encoded[i + 2]);
        int const d = padD ? 0 : sextet(encoded[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (padC && !padD) || ((padC || padD) && !last)
            || (padC && (b & 15) != 0) || (padD && !padC && (c & 3) != 0)) {
            return false;
        }
        auto const value = static_cast<std::uint32_t>((a << 18) | (b << 12) | (c << 6) | d);
        bytes.push_back(static_cast<unsigned char>(value >> 16));
        if (!padC) bytes.push_back(static_cast<unsigned char>(value >> 8));
        if (!padD) bytes.push_back(static_cast<unsigned char>(value));
    }
    constexpr std::array<unsigned char, 8> pngSignature{137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 33 || bytes.size() > kMaxLogoDataBytes) {
        return false;
    }
    for (std::size_t i = 0; i < pngSignature.size(); ++i) {
        if (bytes[i] != pngSignature[i]) return false;
    }
    auto be32 = [&bytes](std::size_t offset) -> std::uint32_t {
        return (std::uint32_t{bytes[offset]} << 24) | (std::uint32_t{bytes[offset + 1]} << 16)
             | (std::uint32_t{bytes[offset + 2]} << 8) | std::uint32_t{bytes[offset + 3]};
    };
    if (be32(8) != 13 || bytes[12] != 'I' || bytes[13] != 'H' || bytes[14] != 'D' || bytes[15] != 'R') {
        return false;
    }
    auto const width = be32(16);
    auto const height = be32(20);
    return width > 0 && height > 0 && width <= 4096 && height <= 4096
        && std::uint64_t{width} * height <= 4'000'000;
}

} // namespace sle::server
