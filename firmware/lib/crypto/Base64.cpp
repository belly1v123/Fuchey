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

} // namespace Crypto
} // namespace Fuchey