#pragma once
// ============================================================
// Fuchey — Ed25519.hpp
// Ed25519 sign and verify.
// Uses mbedTLS 3.x Ed25519 support (via pk context).
// Falls back to a compact public-domain C implementation
// (tweetnacl/donna) if mbedTLS version does not support Ed25519.
//
// For ESP-IDF 5.x with mbedTLS 3.x, Ed25519 is supported via
// MBEDTLS_ECP_DP_CURVE25519 + mbedtls_eddsa_*.
// We use the raw approach for guaranteed compatibility.
// ============================================================

#include <cstdint>
#include <array>
#include <span>

namespace Fuchey {
namespace Crypto {

class Ed25519 {
public:
    static constexpr size_t PRIVATE_KEY_SIZE = 32;
    static constexpr size_t PUBLIC_KEY_SIZE  = 32;
    static constexpr size_t SIGNATURE_SIZE   = 64;

    // ── Generate public key from private key ─────────────
    static bool get_pubkey(std::span<const uint8_t, 32> priv_key,
                           std::span<uint8_t, 32>       out_pubkey);

    // ── Sign ─────────────────────────────────────────────
    // priv_key: 32-byte raw private key (from SLIP0010)
    // pub_key:  32-byte public key
    // message:  arbitrary length message
    // out_sig:  64-byte signature output
    static bool sign(std::span<const uint8_t, 32> priv_key,
                     std::span<const uint8_t, 32> pub_key,
                     std::span<const uint8_t>     message,
                     std::span<uint8_t, 64>        out_sig);

    // ── Verify ───────────────────────────────────────────
    static bool verify(std::span<const uint8_t, 32> pub_key,
                       std::span<const uint8_t>     message,
                       std::span<const uint8_t, 64> signature);

private:
    // Low-level Ed25519 field operations (TweetNaCl-derived)
    // These are static functions with no external state
    static constexpr const char* TAG = "Ed25519";
};

} // namespace Crypto
} // namespace Fuchey
