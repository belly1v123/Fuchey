#include "Base64.hpp"

namespace Fuchey {
namespace Crypto {

static constexpr const char ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64::encode(std::span<const uint8_t> data) {
    std::string result;
    result.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i < data.size()) {
        uint32_t triplet = 0;
        int remaining = 0;

        triplet |= static_cast<uint32_t>(data[i++]) << 16;
        remaining++;
        if (i < data.size()) {
            triplet |= static_cast<uint32_t>(data[i++]) << 8;
            remaining++;
        }
        if (i < data.size()) {
            triplet |= static_cast<uint32_t>(data[i++]);
            remaining++;
        }

        result += ALPHABET[(triplet >> 18) & 0x3F];
        result += ALPHABET[(triplet >> 12) & 0x3F];
        result += (remaining > 1) ? ALPHABET[(triplet >> 6) & 0x3F] : '=';
        result += (remaining > 2) ? ALPHABET[triplet & 0x3F] : '=';
    }

    return result;
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool Base64::decode(std::string_view in, std::vector<uint8_t>& out) {
    out.clear();
    if (in.size() % 4 != 0) return false;
    out.reserve(in.size() / 4 * 3);

    for (size_t i = 0; i < in.size(); i += 4) {
        const bool last = (i + 4 == in.size());
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            char c = in[i + k];
            if (c == '=') {
                // Padding only in the last quantum, only in positions 2-3,
                // and nothing but padding after it.
                if (!last || k < 2) return false;
                v[k] = 0;
                ++pad;
            } else {
                if (pad) return false;
                v[k] = b64_value(c);
                if (v[k] < 0) return false;
            }
        }
        uint32_t triplet = (static_cast<uint32_t>(v[0]) << 18) |
                           (static_cast<uint32_t>(v[1]) << 12) |
                           (static_cast<uint32_t>(v[2]) << 6) |
                            static_cast<uint32_t>(v[3]);
        out.push_back(static_cast<uint8_t>((triplet >> 16) & 0xFF));
        if (pad < 2) out.push_back(static_cast<uint8_t>((triplet >> 8) & 0xFF));
        if (pad < 1) out.push_back(static_cast<uint8_t>(triplet & 0xFF));
    }
    return true;
}

} // namespace Crypto
} // namespace Fuchey