#pragma once
// ============================================================
// Fuchey — SLIP0010.hpp
// SLIP-0010 HD key derivation for Ed25519.
// Solana path: m/44'/501'/0'/0'
//
// SLIP-0010 differs from BIP32:
//  - All derivation is hardened for Ed25519
//  - Uses HMAC-SHA512 with "ed25519 seed" key
// ============================================================

#include "CryptoEngine.hpp"
#include <cstdint>
#include <string>
#include <optional>
#include <vector>
#include <span>

namespace Fuchey {
namespace Crypto {

class SLIP0010 {
public:
    // ── Derive master key from seed ─────────────────────
    // Returns master KeyPair from 64-byte BIP39 seed
    static std::optional<KeyPair> derive_master(const Seed& seed);

    // ── Derive child key (hardened only for Ed25519) ────
    // index: 0x80000000 bit must be set for hardened derivation
    // Ed25519 ONLY supports hardened derivation
    static std::optional<KeyPair> derive_child(const KeyPair& parent,
                                                uint32_t index);

    // ── Parse and derive from path string ───────────────
    // path format: "m/44'/501'/0'/0'"
    // ' indicates hardened derivation
    static std::optional<KeyPair> derive_path(const Seed& seed,
                                               std::string_view path);

    // ── Solana-specific shortcut ─────────────────────────
    // Derives keypair using m/44'/501'/0'/0'
    static std::optional<KeyPair> derive_solana(const Seed& seed);

    // ── Constants ────────────────────────────────────────
    static constexpr uint32_t HARDENED  = 0x80000000u;
    static constexpr uint32_t SOLANA_COIN_TYPE = 501;
    static constexpr uint32_t PURPOSE_44       = 44;

private:
    // Parse path component: "44'" → (44, true), "0" → (0, false)
    static bool parse_component(std::string_view comp,
                                uint32_t& index, bool& hardened);

    // Parse full path into list of (index, hardened) pairs
    static std::optional<std::vector<std::pair<uint32_t, bool>>>
    parse_path(std::string_view path);

    static constexpr const char* CURVE_KEY = "ed25519 seed";
    static constexpr const char* TAG = "SLIP0010";
};

} // namespace Crypto
} // namespace Fuchey
