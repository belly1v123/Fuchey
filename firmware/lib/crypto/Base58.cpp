// ============================================================
// Fuchey — Base58.cpp
// ============================================================

#include "Base58.hpp"
#include "esp_log.h"
#include <algorithm>
#include <cstring>

namespace Fuchey {
namespace Crypto {

static constexpr const char* TAG = "Base58";

// ─── Alphabet lookup ──────────────────────────────────────
int Base58::char_to_val(char c) {
    const char* pos = std::strchr(ALPHABET, c);
    if (!pos) return -1;
    return static_cast<int>(pos - ALPHABET);
}

// ─── Encode ───────────────────────────────────────────────
std::string Base58::encode(std::span<const uint8_t> data) {
    // Count leading zeros
    int leading_zeros = 0;
    for (uint8_t b : data) {
        if (b != 0) break;
        ++leading_zeros;
    }

    // Convert bytes to big integer, then to base58
    // Use std::vector as scratch space
    std::vector<uint8_t> num(data.begin(), data.end());
    std::string result;
    result.reserve(data.size() * 138 / 100 + 1);

    while (!num.empty()) {
        // Divide num by 58, collect remainder
        int rem = 0;
        std::vector<uint8_t> next;
        next.reserve(num.size());
        for (uint8_t byte : num) {
            int val = rem * 256 + byte;
            if (!next.empty() || val / 58 > 0) {
                next.push_back(static_cast<uint8_t>(val / 58));
            }
            rem = val % 58;
        }
        result += ALPHABET[rem];
        num = std::move(next);
    }

    // Add '1' for each leading zero byte
    result.append(leading_zeros, '1');

    // Reverse (we built it backwards)
    std::reverse(result.begin(), result.end());
    return result;
}

// ─── Decode ───────────────────────────────────────────────
std::vector<uint8_t> Base58::decode(std::string_view str) {
    // Count leading '1's
    int leading_ones = 0;
    for (char c : str) {
        if (c != '1') break;
        ++leading_ones;
    }

    std::vector<uint8_t> num;
    num.reserve(str.size());

    for (char c : str) {
        int val = char_to_val(c);
        if (val < 0) {
            ESP_LOGE(TAG, "Invalid Base58 character: %c", c);
            return {};
        }
        // num = num * 58 + val
        int carry = val;
        for (auto it = num.rbegin(); it != num.rend(); ++it) {
            int v = static_cast<int>(*it) * 58 + carry;
            *it = static_cast<uint8_t>(v & 0xFF);
            carry = v >> 8;
        }
        while (carry > 0) {
            num.insert(num.begin(), static_cast<uint8_t>(carry & 0xFF));
            carry >>= 8;
        }
    }

    // Remove leading zeros (from big-int representation)
    auto it = num.begin();
    while (it != num.end() && *it == 0) ++it;
    num.erase(num.begin(), it);

    // Prepend leading zero bytes
    num.insert(num.begin(), leading_ones, 0);
    return num;
}

// ─── Pubkey → Solana address ──────────────────────────────
std::string Base58::pubkey_to_address(std::span<const uint8_t, 32> pubkey) {
    return encode(std::span<const uint8_t>(pubkey.data(), pubkey.size()));
}

// ─── Validate ─────────────────────────────────────────────
bool Base58::is_valid_address(std::string_view addr) {
    if (addr.empty() || addr.size() > 44) return false;
    for (char c : addr) {
        if (char_to_val(c) < 0) return false;
    }
    // Solana addresses are 32-byte pubkeys → encode to ~44 chars
    auto decoded = decode(addr);
    return decoded.size() == 32;
}

} // namespace Crypto
} // namespace Fuchey
