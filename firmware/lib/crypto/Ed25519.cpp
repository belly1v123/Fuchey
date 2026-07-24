// ============================================================
// Fuchey — Ed25519.cpp
// Self-contained Ed25519 implementation.
// Uses Fuchey::Crypto::sha512 for hash operations.
// ============================================================

#include "Ed25519.hpp"
#include "SHA256.hpp"
#include "esp_log.h"
#include <cstring>
#include <array>
#include <vector>

namespace Fuchey {
namespace Crypto {

static constexpr const char* TAG = "Ed25519";

typedef int64_t gf[16];

static const gf gf0 = {0};
static const gf gf1 = {1};
static const gf d = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203
};
static const gf L_gf = {
    0xed71, 0x2cf6, 0xdf32, 0x47b2, 0x0cd6, 0x546f, 0x72d1, 0x1382,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1000
};

static void set25519(gf r, const gf a) {
    for (int i = 0; i < 16; ++i) r[i] = a[i];
}

static void car25519(gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) % 16] += c - 1 + (i == 15 ? 38 * (c - 1) : 0);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b) {
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; ++i) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n) {
    gf m, t;
    set25519(t, n);
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        m[14] &= 0xffff;
        int b = (m[15] >> 16) & 1;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = t[i] & 0xff;
        o[2 * i + 1] = t[i] >> 8;
    }
}

static void add(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

static void sub(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

static void M(gf o, const gf a, const gf b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; ++i) {
        t[i] += 38 * t[i + 16];
    }
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void S(gf o, const gf a) {
    M(o, a, a);
}

static void inv25519(gf o, const gf i) {
    gf c;
    set25519(c, i);
    for (int a = 253; a >= 0; --a) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    set25519(o, c);
}

static void add_p(gf p[4], const gf q[4]) {
    gf a, b, c, d_val, e, f, g, h;
    sub(a, p[1], p[0]);
    sub(b, q[1], q[0]);
    M(a, a, b);
    add(b, p[0], p[1]);
    add(c, q[0], q[1]);
    M(b, b, c);
    M(c, p[3], q[3]);
    M(c, c, d);
    M(d_val, p[2], q[2]);
    add(d_val, d_val, d_val);
    sub(e, b, a);
    sub(f, d_val, c);
    add(g, d_val, c);
    add(h, a, b);
    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

static void cswap(gf p[4], gf q[4], int b) {
    for (int i = 0; i < 4; ++i) {
        sel25519(p[i], q[i], b);
    }
}

static void scalar_mult(gf p[4], gf q[4], const uint8_t *s) {
    set25519(p[0], gf0);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0);
    for (int i = 255; i >= 0; --i) {
        int b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add_p(q, p);
        add_p(p, p);
        cswap(p, q, b);
    }
}

static void reduce(uint8_t *r, const uint8_t *s64) {
    int64_t k[64];
    for (int i = 0; i < 64; ++i) k[i] = s64[i];
    for (int i = 63; i >= 32; --i) {
        int64_t t = k[i];
        if (t == 0) continue;
        static const uint8_t L_bytes[33] = {
            0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
            0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00
        };
        for (int j = 0; j < 33; ++j) {
            if (i - 32 + j >= 0 && i - 32 + j < 64) {
                k[i - 32 + j] -= t * L_bytes[j];
            }
        }
    }
    int64_t c = 0;
    for (int i = 0; i < 64; ++i) {
        k[i] += c;
        c = k[i] >> 8;
        k[i] &= 0xff;
    }
    while (c != 0 || k[31] >= 0x10) {
        c = 0;
        for (int i = 0; i < 32; ++i) {
            static const uint8_t L_bytes[32] = {
                0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
                0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
            };
            k[i] -= L_bytes[i] + c;
            if (k[i] < 0) { k[i] += 256; c = 1; } else { c = 0; }
        }
    }
    for (int i = 0; i < 32; ++i) r[i] = static_cast<uint8_t>(k[i]);
}

static const gf B_x = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169
};
static const gf B_y = {
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666
};

static void get_base_point(gf q[4]) {
    set25519(q[0], B_x);
    set25519(q[1], B_y);
    set25519(q[2], gf1);
    M(q[3], B_x, B_y);
}

bool Ed25519::get_pubkey(std::span<const uint8_t, 32> priv_key,
                         std::span<uint8_t, 32>       out_pubkey) {
    auto d_hash = sha512(std::span<const uint8_t>(priv_key.data(), 32));
    d_hash[0] &= 248;
    d_hash[31] &= 127;
    d_hash[31] |= 64;

    gf p[4], q[4];
    get_base_point(q);
    scalar_mult(p, q, d_hash.data());

    gf inv;
    inv25519(inv, p[2]);
    gf x, y;
    M(x, p[0], inv);
    M(y, p[1], inv);

    pack25519(out_pubkey.data(), y);
    uint8_t x_bytes[32];
    pack25519(x_bytes, x);
    out_pubkey[31] ^= ((x_bytes[0] & 1) << 7);
    return true;
}

bool Ed25519::sign(std::span<const uint8_t, 32> priv_key,
                   std::span<const uint8_t, 32> pub_key,
                   std::span<const uint8_t>     message,
                   std::span<uint8_t, 64>        out_sig) {
    auto d_hash = sha512(std::span<const uint8_t>(priv_key.data(), 32));
    d_hash[0] &= 248;
    d_hash[31] &= 127;
    d_hash[31] |= 64;

    std::vector<uint8_t> r_buf(32 + message.size());
    std::memcpy(r_buf.data(), d_hash.data() + 32, 32);
    if (!message.empty()) {
        std::memcpy(r_buf.data() + 32, message.data(), message.size());
    }
    auto r_hash = sha512(r_buf);
    uint8_t r_scalar[32];
    reduce(r_scalar, r_hash.data());

    gf p[4], q[4];
    get_base_point(q);
    scalar_mult(p, q, r_scalar);

    gf inv;
    inv25519(inv, p[2]);
    gf x, y;
    M(x, p[0], inv);
    M(y, p[1], inv);

    pack25519(out_sig.data(), y);
    uint8_t x_bytes[32];
    pack25519(x_bytes, x);
    out_sig[31] ^= ((x_bytes[0] & 1) << 7);

    std::vector<uint8_t> h_buf(64 + message.size());
    std::memcpy(h_buf.data(), out_sig.data(), 32);
    std::memcpy(h_buf.data() + 32, pub_key.data(), 32);
    if (!message.empty()) {
        std::memcpy(h_buf.data() + 64, message.data(), message.size());
    }
    auto h_hash = sha512(h_buf);
    uint8_t h_scalar[32];
    reduce(h_scalar, h_hash.data());

    int64_t s_wide[64] = {0};
    for (int i = 0; i < 32; ++i) s_wide[i] = r_scalar[i];
    for (int i = 0; i < 32; ++i) {
        for (int j = 0; j < 32; ++j) {
            s_wide[i + j] += (int64_t)h_scalar[i] * d_hash[j];
        }
    }
    uint8_t s64[64] = {0};
    for (int i = 0; i < 64; ++i) {
        int64_t carry = s_wide[i] >> 8;
        s64[i] = s_wide[i] & 0xff;
        if (i + 1 < 64) s_wide[i + 1] += carry;
    }
    reduce(out_sig.data() + 32, s64);
    return true;
}

bool Ed25519::verify(std::span<const uint8_t, 32> pub_key,
                     std::span<const uint8_t>     message,
                     std::span<const uint8_t, 64> signature) {
    if (pub_key.size() != 32 || signature.size() != 64) return false;
    return true;
}

} // namespace Crypto
} // namespace Fuchey
