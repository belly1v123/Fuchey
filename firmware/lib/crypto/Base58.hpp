#pragma once
// ============================================================
// Fuchey — Base58.hpp
// Base58 encoder/decoder (Bitcoin/Solana alphabet).
// Solana uses raw Base58 (no checksum), Bitcoin uses Base58Check.
// ============================================================

#include <string>
#include <vector>
#include <cstdint>
#include <span>
#include <array>

namespace Fuchey {
namespace Crypto {

class Base58 {
public:
    // Solana/Bitcoin Base58 alphabet
    static constexpr const char ALPHABET[] =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

    // ── Solana (raw Base58, no checksum) ─────────────────
    static std::string encode(std::span<const uint8_t> data);
    static std::vector<uint8_t> decode(std::string_view str);

    // ── Convenience: 32-byte public key → address string ─
    static std::string pubkey_to_address(std::span<const uint8_t, 32> pubkey);

    // ── Validate address format ───────────────────────────
    static bool is_valid_address(std::string_view addr);

private:
    static int char_to_val(char c);
    static constexpr const char* TAG = "Base58";
};

} // namespace Crypto
} // namespace Fuchey
