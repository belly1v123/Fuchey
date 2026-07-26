#pragma once
// ============================================================
// Fuchey — SHA256.hpp
// Thin wrapper around mbedTLS SHA-256 and HMAC-SHA512.
// No dynamic allocation. Caller provides buffers.
// ============================================================

#ifndef MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#endif

#include <mbedtls/private/sha256.h>
#include <mbedtls/private/sha512.h>
#include <mbedtls/md.h>
#include <mbedtls/private/pkcs5.h>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <array>
#include <span>

namespace Fuchey {
namespace Crypto {

static constexpr size_t SHA512_BLOCK_SIZE = 128;

// ─── SHA-256 ──────────────────────────────────────────────
inline std::array<uint8_t, 32> sha256(std::span<const uint8_t> data) {
    std::array<uint8_t, 32> out{};
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md(md_info, data.data(), data.size(), out.data());
    return out;
}

// Double SHA-256 (used in BIP39 checksum)
inline std::array<uint8_t, 32> sha256d(std::span<const uint8_t> data) {
    auto first = sha256(data);
    return sha256(std::span<const uint8_t>(first.data(), first.size()));
}

// ─── SHA-512 ──────────────────────────────────────────────
inline std::array<uint8_t, 64> sha512(std::span<const uint8_t> data) {
    std::array<uint8_t, 64> out{};
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    mbedtls_md(md_info, data.data(), data.size(), out.data());
    return out;
}

// ─── HMAC-SHA512 (RFC 2104) ──────────────────────────────
// Manual implementation using the working sha512() function,
// avoiding a bug in mbedTLS's HMAC context on this platform.
inline std::array<uint8_t, 64> hmac_sha512(
    std::span<const uint8_t> key,
    std::span<const uint8_t> data)
{
    std::array<uint8_t, SHA512_BLOCK_SIZE> key_pad{};
    std::array<uint8_t, SHA512_BLOCK_SIZE> ipad{};
    std::array<uint8_t, SHA512_BLOCK_SIZE> opad{};

    // If key is longer than block size, hash it first
    std::array<uint8_t, 64> hashed_key{};
    std::span<const uint8_t> k = key;
    if (key.size() > SHA512_BLOCK_SIZE) {
        hashed_key = sha512(key);
        k = std::span<const uint8_t>(hashed_key.data(), hashed_key.size());
    }

    // Copy key into block-sized buffer and XOR with ipad/opad
    std::memcpy(key_pad.data(), k.data(), k.size());
    for (size_t i = 0; i < SHA512_BLOCK_SIZE; ++i) {
        ipad[i] = key_pad[i] ^ 0x36;
        opad[i] = key_pad[i] ^ 0x5c;
    }

    // Inner: SHA512(ipad || data)
    std::array<uint8_t, 64> inner{};
    {
        std::array<uint8_t, SHA512_BLOCK_SIZE + 256> inner_buf{};
        std::memcpy(inner_buf.data(), ipad.data(), SHA512_BLOCK_SIZE);
        std::memcpy(inner_buf.data() + SHA512_BLOCK_SIZE, data.data(), data.size());
        inner = sha512(std::span<const uint8_t>(inner_buf.data(), SHA512_BLOCK_SIZE + data.size()));
    }

    // Outer: SHA512(opad || inner)
    std::array<uint8_t, SHA512_BLOCK_SIZE + 64> outer_buf{};
    std::memcpy(outer_buf.data(), opad.data(), SHA512_BLOCK_SIZE);
    std::memcpy(outer_buf.data() + SHA512_BLOCK_SIZE, inner.data(), inner.size());
    return sha512(std::span<const uint8_t>(outer_buf.data(), SHA512_BLOCK_SIZE + inner.size()));
}

// ─── PBKDF2-HMAC-SHA512 (used in BIP39 mnemonic→seed) ────
// Produces 'out_len' bytes of key material
// Standard BIP39 uses: PBKDF2(mnemonic, "mnemonic" + passphrase, 2048, 64)
inline bool pbkdf2_hmac_sha512(
    std::span<const uint8_t> password,
    std::span<const uint8_t> salt,
    uint32_t                 iterations,
    std::span<uint8_t>       out)
{
    int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA512,
        password.data(), password.size(),
        salt.data(),     salt.size(),
        iterations,
        static_cast<uint32_t>(out.size()),
        out.data()
    );
    return ret == 0;
}

} // namespace Crypto
} // namespace Fuchey
