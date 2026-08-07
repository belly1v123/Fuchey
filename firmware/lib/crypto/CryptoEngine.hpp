#pragma once
// ============================================================
// Fuchey — CryptoEngine.hpp
// Top-level crypto interface. No network, no UI.
// Aggregates: BIP39, SLIP0010, Ed25519, Base58, SHA256.
//
// SECURITY RULES:
//   - Never log key material
//   - Zero sensitive buffers after use
//   - No heap allocation of key material
//   - All outputs go through caller-provided buffers
// ============================================================

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <array>
#include <span>

namespace Fuchey {
namespace Crypto {

// ─── Type aliases for clarity ─────────────────────────────
using PrivKey   = std::array<uint8_t, 32>;
using PubKey    = std::array<uint8_t, 32>;
using Signature = std::array<uint8_t, 64>;
using Seed      = std::array<uint8_t, 64>;
using SecretKey = std::array<uint8_t, 64>; // Solana 64-byte secret key (priv32 || pub32)
using ChainCode = std::array<uint8_t, 32>;

// ─── Key pair ─────────────────────────────────────────────
struct KeyPair {
    PrivKey  priv_key;
    PubKey   pub_key;
    ChainCode chain_code;

    // Explicit zero on destruction
    ~KeyPair() {
        priv_key.fill(0);
        chain_code.fill(0);
        // pub_key is public — no need to zero
    }
    // Non-copyable — key material should not be copied accidentally
    KeyPair(const KeyPair&) = delete;
    KeyPair& operator=(const KeyPair&) = delete;
    KeyPair(KeyPair&&) = default;
    KeyPair& operator=(KeyPair&&) = default;
    KeyPair() = default;
};

// ─── CryptoEngine ─────────────────────────────────────────
class CryptoEngine {
public:
    // ── BIP39 ────────────────────────────────────────────

    // Generate a cryptographically random mnemonic
    // words: 12 or 24
    static std::optional<std::string> generate_mnemonic(int words = 24);

    // Validate a mnemonic string (checksum + word list check)
    static bool validate_mnemonic(std::string_view mnemonic);

    // Convert mnemonic + optional passphrase to 64-byte seed
    // The seed is placed in 'out_seed'. Returns true on success.
    // SECURITY: passphrase may be empty string
    static bool mnemonic_to_seed(std::string_view mnemonic,
                                  std::string_view passphrase,
                                  Seed& out_seed);

    // ── SLIP0010 / HD Key Derivation ─────────────────────

    // Derive keypair from seed using Solana path: m/44'/501'/0'/0'
    // Returns std::nullopt on failure
    static std::optional<KeyPair> derive_solana_keypair(const Seed& seed);

    // Generic path derivation (for testing / advanced use)
    static std::optional<KeyPair> derive_keypair(const Seed& seed,
                                                   std::string_view path);

    // ── Ed25519 ──────────────────────────────────────────

    // Sign message bytes with private key
    // SECURITY: priv_key never leaves this module
    static bool sign(const PrivKey& priv_key,
                     const PubKey&  pub_key,
                     std::span<const uint8_t> message,
                     Signature& out_sig);

    // Verify a signature
    static bool verify(const PubKey& pub_key,
                       std::span<const uint8_t> message,
                       const Signature& sig);

    // ── Base58 ───────────────────────────────────────────

    // Encode public key to Solana base58 address string
    static std::string pubkey_to_address(const PubKey& pub_key);

    // Decode base58 address to pubkey bytes (returns false if invalid)
    static bool address_to_pubkey(std::string_view address, PubKey& out);

    // ── SHA256 ───────────────────────────────────────────
    static std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);
    static std::array<uint8_t, 32> sha256d(std::span<const uint8_t> data); // double SHA256

private:
    static constexpr const char* TAG = "CryptoEngine";
};

} // namespace Crypto
} // namespace Fuchey
