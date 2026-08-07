// ============================================================
// Fuchey — SLIP0010.cpp
// SLIP-0010 implementation for Ed25519 (Solana)
// ============================================================

#include "SLIP0010.hpp"
#include "SHA256.hpp"
#include "esp_log.h"
#include <cstring>
#include <charconv>

namespace Fuchey {
namespace Crypto {

static constexpr const char* TAG = "SLIP0010";

// ─── Derive master ────────────────────────────────────────
// SLIP0010: I = HMAC-SHA512(Key="ed25519 seed", Data=seed)
// IL = private key, IR = chain code
std::optional<KeyPair> SLIP0010::derive_master(const Seed& seed) {
    static const uint8_t curve_key[] = "ed25519 seed";
    auto I = hmac_sha512(
        std::span<const uint8_t>(curve_key, sizeof(curve_key) - 1),
        std::span<const uint8_t>(seed.data(), seed.size())
    );

    KeyPair kp;
    // IL = first 32 bytes → private key
    std::copy(I.begin(),        I.begin() + 32, kp.priv_key.begin());
    // IR = last 32 bytes → chain code
    std::copy(I.begin() + 32,   I.end(),        kp.chain_code.begin());

    // Zero I
    I.fill(0);
    return kp;
}

// ─── Derive child (hardened only) ────────────────────────
// SLIP0010 Ed25519: I = HMAC-SHA512(Key=chain_code, Data=0x00||IL||index_be)
std::optional<KeyPair> SLIP0010::derive_child(const KeyPair& parent,
                                               uint32_t index) {
    // Ed25519 only supports hardened derivation
    if (!(index & HARDENED)) {
        // Auto-harden
        index |= HARDENED;
        ESP_LOGD(TAG, "Auto-hardening index 0x%08X for Ed25519", index);
    }

    // Data = 0x00 || parent_private_key || index (big-endian)
    std::array<uint8_t, 37> data{};
    data[0] = 0x00;
    std::copy(parent.priv_key.begin(), parent.priv_key.end(), data.begin() + 1);
    data[33] = (index >> 24) & 0xFF;
    data[34] = (index >> 16) & 0xFF;
    data[35] = (index >>  8) & 0xFF;
    data[36] = (index      ) & 0xFF;

    auto I = hmac_sha512(
        std::span<const uint8_t>(parent.chain_code.data(), parent.chain_code.size()),
        std::span<const uint8_t>(data.data(), data.size())
    );

    // Zero data
    data.fill(0);

    KeyPair child;
    std::copy(I.begin(),      I.begin() + 32, child.priv_key.begin());
    std::copy(I.begin() + 32, I.end(),        child.chain_code.begin());
    I.fill(0);

    return child;
}

// ─── Parse path component ─────────────────────────────────
bool SLIP0010::parse_component(std::string_view comp,
                                uint32_t& index, bool& is_hardened) {
    is_hardened = false;
    if (!comp.empty() && comp.back() == '\'') {
        is_hardened = true;
        comp = comp.substr(0, comp.size() - 1);
    }

    uint32_t val = 0;
    auto [ptr, ec] = std::from_chars(comp.data(), comp.data() + comp.size(), val);
    if (ec != std::errc{}) return false;
    index = val;
    return true;
}

// ─── Parse full path ──────────────────────────────────────
std::optional<std::vector<std::pair<uint32_t, bool>>>
SLIP0010::parse_path(std::string_view path) {
    if (path.empty() || path[0] != 'm') return std::nullopt;
    path = path.substr(1); // skip 'm'

    std::vector<std::pair<uint32_t, bool>> result;

    while (!path.empty()) {
        if (path[0] == '/') path = path.substr(1);
        if (path.empty()) break;

        size_t sep = path.find('/');
        auto comp = path.substr(0, sep);
        path = (sep == std::string_view::npos) ? "" : path.substr(sep);

        uint32_t idx = 0;
        bool hardened = false;
        if (!parse_component(comp, idx, hardened)) {
            ESP_LOGE(TAG, "Failed to parse path component");
            return std::nullopt;
        }
        result.emplace_back(idx, hardened);
    }
    return result;
}

// ─── Derive from path ─────────────────────────────────────
std::optional<KeyPair> SLIP0010::derive_path(const Seed& seed,
                                              std::string_view path) {
    auto components = parse_path(path);
    if (!components) return std::nullopt;

    auto kp = derive_master(seed);
    if (!kp) return std::nullopt;

    for (auto& [idx, hardened] : *components) {
        uint32_t child_index = idx | (hardened ? HARDENED : 0);
        auto child = derive_child(*kp, child_index);
        if (!child) return std::nullopt;
        kp = std::move(child);
    }
    return kp;
}

// ─── Solana shortcut ─────────────────────────────────────
std::optional<KeyPair> SLIP0010::derive_solana(const Seed& seed) {
    // m/44'/501'/0'/0'
    return derive_path(seed, "m/44'/501'/0'/0'");
}

} // namespace Crypto
} // namespace Fuchey
