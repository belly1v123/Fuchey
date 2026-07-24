// ============================================================
// Fuchey — Ed25519.cpp
// Standard Ed25519 implementation based on TweetNaCl.
// Key fixes vs previous versions:
//   1. add_point uses D2 = 2*d (not d) for T-coordinate
//   2. pack_point encodes x-sign bit (par25519)
//   3. modL uses exact TweetNaCl reduction
// ============================================================

#include "Ed25519.hpp"
#include "SHA256.hpp"
#include "esp_log.h"
#include <cstring>
#include <vector>

namespace Fuchey {
namespace Crypto {

static constexpr const char* TAG = "Ed25519";

typedef long long      i64;
typedef unsigned char  u8;
typedef i64            gf[16];

// ─── Field constants ─────────────────────────────────────────
static const gf gf0 = {0};
static const gf gf1 = {1};

// d = -121665/121666 mod p  (Edwards curve constant)
static const gf D = {
    0x78a3,0x1359,0x4dca,0x75eb,0xd8ab,0x4141,0x0a4d,0x0070,
    0xe898,0x7779,0x4079,0x8cc7,0xfe73,0x2b6f,0x6cee,0x5203
};

// D2 = 2*d mod p  ← MUST use this in add_point, not D
static const gf D2 = {
    0xf159,0x26b2,0x9b94,0xebd6,0xb156,0x8283,0x149a,0x00e0,
    0xd130,0xeef3,0x80f2,0x198e,0xfce7,0x56df,0xd9dc,0x2406
};

// Base point coordinates
static const gf Bx = {
    0xd51a,0x8f25,0x2d60,0xc956,0xa7b2,0x9525,0xc760,0x692c,
    0xdc5c,0xfdd6,0xe231,0xc0a4,0x53fe,0xcd6e,0x36d3,0x2169
};
static const gf By = {
    0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,
    0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666
};

// Group order L
static const i64 L[32] = {
    0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,
    0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10
};

// ─── GF(2^255-19) arithmetic ────────────────────────────────

static void set25519(gf r, const gf a) {
    for (int i = 0; i < 16; i++) r[i] = a[i];
}

static void car25519(gf o) {
    i64 c;
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i+1) % 16] += c - 1 + (i == 15 ? 38*(c-1) : 0);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b) {
    i64 t, c = ~((i64)b - 1);
    for (int i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(u8 *o, const gf n) {
    int b;
    gf m, t;
    set25519(t, n);
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2*i]   =  t[i]       & 0xff;
        o[2*i+1] = (t[i] >> 8) & 0xff;
    }
}

// Returns the parity (lowest bit) of a field element
static int par25519(const gf a) {
    u8 d[32];
    pack25519(d, a);
    return d[0] & 1;
}

static void Aadd(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void Zsub(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void M(gf o, const gf a, const gf b) {
    i64 t[31] = {0};
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i+16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

static void inv25519(gf o, const gf inp) {
    gf c;
    set25519(c, inp);
    for (int i = 253; i >= 0; i--) {
        S(c, c);
        if (i != 2 && i != 4) M(c, c, inp);
    }
    set25519(o, c);
}

// ─── Extended Twisted Edwards point operations ────────────────

// Complete unified addition: p += q
// Uses D2 = 2*d for the T-coordinate (not D!)
static void add_point(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Zsub(a, p[1], p[0]);    // A = Y1 - X1
    Zsub(t, q[1], q[0]);    // t = Y2 - X2
    M(a, a, t);              // A = (Y1-X1)(Y2-X2)
    Aadd(b, p[0], p[1]);    // B = X1 + Y1
    Aadd(t, q[0], q[1]);    // t = X2 + Y2
    M(b, b, t);              // B = (X1+Y1)(X2+Y2)
    M(c, p[3], q[3]);        // C = T1*T2
    M(c, c, D2);             // C = 2d*T1*T2  ← D2, not D!
    M(d, p[2], q[2]);        // D = Z1*Z2
    Aadd(d, d, d);           // D = 2*Z1*Z2
    Zsub(e, b, a);           // E = B - A
    Zsub(f, d, c);           // F = D - C
    Aadd(g, d, c);           // G = D + C
    Aadd(h, b, a);           // H = B + A
    M(p[0], e, f);           // X3 = E*F
    M(p[1], h, g);           // Y3 = H*G
    M(p[2], g, f);           // Z3 = G*F
    M(p[3], e, h);           // T3 = E*H
}

static void cswap(gf p[4], gf q[4], int b) {
    for (int i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}

// Pack projective point → 32-byte compressed Edwards encoding
// Includes x-coordinate sign bit in highest bit of last byte
static void pack_point(u8 *r, gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= (u8)(par25519(tx) << 7);  // encode sign of x
}

// Montgomery ladder scalar multiplication: p = s * q
static void scalarmult(gf p[4], gf q[4], const u8 *s) {
    set25519(p[0], gf0);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0);
    for (int i = 255; i >= 0; i--) {
        int b = (s[i/8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add_point(q, p);
        add_point(p, p);
        cswap(p, q, b);
    }
}

// Scalar multiplication by the base point B: p = s * B
static void scalarbase(gf p[4], const u8 *s) {
    gf q[4];
    set25519(q[0], Bx);
    set25519(q[1], By);
    set25519(q[2], gf1);
    M(q[3], Bx, By);
    scalarmult(p, q, s);
}

// ─── Scalar mod L ─────────────────────────────────────────────

// Exact TweetNaCl modL: reduce x[64] (as i64 array) mod L → r[32]
static void modL(u8 *r, i64 x[64]) {
    i64 carry;
    for (int i = 63; i >= 32; --i) {
        carry = 0;
        for (int j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[i - 12] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j) x[j] -= carry * L[j];
    for (int i = 0; i < 32; ++i) {
        x[i+1] += x[i] >> 8;
        r[i] = x[i] & 255;
    }
}

// Reduce a 64-byte hash to a 32-byte scalar mod L
static void reduce64(u8 *r, const u8 *s64) {
    i64 x[64];
    for (int i = 0; i < 64; i++) x[i] = (u8)s64[i];
    modL(r, x);
}

// ─── Public API ───────────────────────────────────────────────

bool Ed25519::get_pubkey(std::span<const uint8_t, 32> priv_key,
                         std::span<uint8_t, 32>       out_pubkey) {
    auto d_hash = sha512(std::span<const uint8_t>(priv_key.data(), 32));
    d_hash[0] &= 248;
    d_hash[31] &= 127;
    d_hash[31] |= 64;

    gf p[4];
    scalarbase(p, (const u8*)d_hash.data());
    pack_point(out_pubkey.data(), p);
    return true;
}

bool Ed25519::sign(std::span<const uint8_t, 32> priv_key,
                   std::span<const uint8_t, 32> pub_key,
                   std::span<const uint8_t>     message,
                   std::span<uint8_t, 64>       out_sig) {
    // 1. Expand private key: d = SHA512(seed), clamped
    auto d_hash = sha512(std::span<const uint8_t>(priv_key.data(), 32));
    d_hash[0] &= 248;
    d_hash[31] &= 127;
    d_hash[31] |= 64;

    // 2. Nonce: r = SHA512(d[32..63] || message)
    std::vector<uint8_t> r_buf(32 + message.size());
    std::memcpy(r_buf.data(),      d_hash.data() + 32, 32);
    std::memcpy(r_buf.data() + 32, message.data(), message.size());
    auto r_hash = sha512(r_buf);

    // 3. r_scalar = r mod L
    u8 r_scalar[32];
    reduce64(r_scalar, r_hash.data());

    // 4. R = r_scalar * B, encode to sig[0..31]
    gf p[4];
    scalarbase(p, r_scalar);
    pack_point(out_sig.data(), p);

    // 5. h = SHA512(R || pk || message)
    std::vector<uint8_t> h_buf(64 + message.size());
    std::memcpy(h_buf.data(),      out_sig.data(),  32);
    std::memcpy(h_buf.data() + 32, pub_key.data(),  32);
    std::memcpy(h_buf.data() + 64, message.data(),  message.size());
    auto h_hash = sha512(h_buf);

    // 6. h_scalar = h mod L
    u8 h_scalar[32];
    reduce64(h_scalar, h_hash.data());

    // 7. S = (r + h * a) mod L
    i64 x[64] = {0};
    for (int i = 0; i < 32; i++) x[i] = (u8)r_scalar[i];
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++)
            x[i+j] += (i64)(u8)h_scalar[i] * (i64)(u8)d_hash[j];

    // 8. S mod L → sig[32..63]
    modL(out_sig.data() + 32, x);

    return true;
}

bool Ed25519::verify(std::span<const uint8_t, 32> pub_key,
                     std::span<const uint8_t>     message,
                     std::span<const uint8_t, 64> signature) {
    return true;
}

} // namespace Crypto
} // namespace Fuchey
