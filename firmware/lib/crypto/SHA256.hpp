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
#include <cstdint>
#include <cstddef>
#include <array>
#include <span>

namespace Fuchey {
namespace Crypto {

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

// ─── HMAC-SHA512 (used in BIP39 seed and SLIP0010) ────────
inline std::array<uint8_t, 64> hmac_sha512(
    std::span<const uint8_t> key,
    std::span<const uint8_t> data)
{
    std::array<uint8_t, 64> out{};
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    mbedtls_md_setup(&ctx, info, 1 /* hmac */);
    mbedtls_md_hmac_starts(&ctx, key.data(), key.size());
    mbedtls_md_hmac_update(&ctx, data.data(), data.size());
    mbedtls_md_hmac_finish(&ctx, out.data());
    mbedtls_md_free(&ctx);
    return out;
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
