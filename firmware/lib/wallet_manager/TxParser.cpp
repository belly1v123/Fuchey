// ============================================================
// Fuchey — TxParser.cpp
// ============================================================

#include "TxParser.hpp"
#include "../crypto/Base58.hpp"
#include "../config/Config.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

namespace Fuchey {
namespace TxParser {

namespace {

constexpr uint64_t LAMPORTS_PER_SIGNATURE   = 5000;
constexpr uint32_t DEFAULT_CU_PER_INSTRUCTION = 200000;
constexpr uint32_t MAX_CU_LIMIT             = 1400000;

constexpr uint32_t SYSTEM_IX_TRANSFER       = 2;
constexpr uint8_t  TOKEN_IX_TRANSFER_CHECKED = 12;
constexpr uint8_t  CB_IX_REQUEST_HEAP_FRAME = 1;
constexpr uint8_t  CB_IX_SET_CU_LIMIT       = 2;
constexpr uint8_t  CB_IX_SET_CU_PRICE       = 3;
constexpr uint8_t  CB_IX_SET_DATA_SIZE      = 4;

// Decoded once; the constants are valid base58 so decode cannot fail.
struct Known {
    Crypto::PubKey token_program{};
    Crypto::PubKey compute_budget{};
    Crypto::PubKey usdc_mainnet{};
    Crypto::PubKey usdc_devnet{};
};

bool decode_key(const char* b58, Crypto::PubKey& out) {
    auto bytes = Crypto::Base58::decode(b58);
    if (bytes.size() != out.size()) return false;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return true;
}

const Known& known() {
    static Known k = [] {
        Known v;
        decode_key("TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA", v.token_program);
        decode_key("ComputeBudget111111111111111111111111111111", v.compute_budget);
        decode_key(API::USDC_MAINNET_MINT, v.usdc_mainnet);
        decode_key(API::USDC_DEVNET_MINT, v.usdc_devnet);
        return v;
    }();
    return k;
}

class Reader {
public:
    explicit Reader(std::span<const uint8_t> d) : m_d(d) {}

    bool u8(uint8_t& v) {
        if (m_pos + 1 > m_d.size()) return false;
        v = m_d[m_pos++];
        return true;
    }
    // Solana "shortvec" (compact-u16): 1-3 bytes, 7 bits each.
    bool compact_u16(uint16_t& v) {
        uint32_t result = 0;
        for (int i = 0; i < 3; ++i) {
            uint8_t b;
            if (!u8(b)) return false;
            result |= static_cast<uint32_t>(b & 0x7F) << (7 * i);
            if ((b & 0x80) == 0) {
                if (result > 0xFFFF) return false;
                v = static_cast<uint16_t>(result);
                return true;
            }
        }
        return false;
    }
    bool bytes(size_t n, std::span<const uint8_t>& out) {
        if (m_pos + n > m_d.size()) return false;
        out = m_d.subspan(m_pos, n);
        m_pos += n;
        return true;
    }
    bool at_end() const { return m_pos == m_d.size(); }

private:
    std::span<const uint8_t> m_d;
    size_t m_pos{0};
};

uint64_t read_u64_le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

ParseError parse_transfer(std::span<const uint8_t> message,
                          const Crypto::PubKey& signer,
                          ParsedTransfer& out) {
    const Known& k = known();
    Reader r(message);
    out = ParsedTransfer{};

    // ── Header ───────────────────────────────────────────
    uint8_t num_sigs, ro_signed, ro_unsigned;
    if (!r.u8(num_sigs)) return ParseError::TRUNCATED;
    if (num_sigs & 0x80) return ParseError::VERSIONED_UNSUPPORTED;
    if (!r.u8(ro_signed) || !r.u8(ro_unsigned)) return ParseError::TRUNCATED;
    // Only this wallet signs, and as a writable fee payer.
    if (num_sigs != 1 || ro_signed != 0) return ParseError::BAD_HEADER;

    // ── Account keys ─────────────────────────────────────
    uint16_t num_keys;
    if (!r.compact_u16(num_keys)) return ParseError::TRUNCATED;
    if (num_keys < 2 || ro_unsigned > num_keys - num_sigs) return ParseError::BAD_HEADER;

    std::span<const uint8_t> keys;
    if (!r.bytes(static_cast<size_t>(num_keys) * 32, keys)) return ParseError::TRUNCATED;
    auto key_at = [&](uint16_t i) { return keys.subspan(static_cast<size_t>(i) * 32, 32); };
    auto key_eq = [&](uint16_t i, const Crypto::PubKey& pk) {
        return std::equal(pk.begin(), pk.end(), key_at(i).begin());
    };
    auto is_zero_key = [&](uint16_t i) {
        auto kk = key_at(i);
        return std::all_of(kk.begin(), kk.end(), [](uint8_t b) { return b == 0; });
    };
    auto is_writable = [&](uint16_t i) {
        if (i < num_sigs) return i < num_sigs - ro_signed;
        return i < num_keys - ro_unsigned;
    };

    if (!key_eq(0, signer)) return ParseError::FEE_PAYER_MISMATCH;

    // ── Recent blockhash ─────────────────────────────────
    std::span<const uint8_t> blockhash;
    if (!r.bytes(32, blockhash)) return ParseError::TRUNCATED;

    // ── Instructions ─────────────────────────────────────
    uint16_t num_ix;
    if (!r.compact_u16(num_ix)) return ParseError::TRUNCATED;

    bool     have_transfer = false;
    bool     cu_limit_set  = false;
    uint32_t cu_limit      = 0;
    uint64_t cu_price      = 0;  // micro-lamports per CU
    uint32_t non_cb_ix     = 0;

    for (uint16_t ix = 0; ix < num_ix; ++ix) {
        uint8_t prog;
        if (!r.u8(prog)) return ParseError::TRUNCATED;
        if (prog >= num_keys || prog == 0) return ParseError::BAD_ACCOUNT_INDEX;

        uint16_t n_acc;
        if (!r.compact_u16(n_acc)) return ParseError::TRUNCATED;
        std::span<const uint8_t> acc;
        if (!r.bytes(n_acc, acc)) return ParseError::TRUNCATED;
        for (uint8_t a : acc) {
            if (a >= num_keys) return ParseError::BAD_ACCOUNT_INDEX;
        }

        uint16_t data_len;
        if (!r.compact_u16(data_len)) return ParseError::TRUNCATED;
        std::span<const uint8_t> data;
        if (!r.bytes(data_len, data)) return ParseError::TRUNCATED;

        if (key_eq(prog, k.compute_budget)) {
            if (data.empty()) return ParseError::UNSUPPORTED_INSTRUCTION;
            switch (data[0]) {
                case CB_IX_SET_CU_LIMIT:
                    if (data.size() != 5) return ParseError::UNSUPPORTED_INSTRUCTION;
                    cu_limit = read_u32_le(data.data() + 1);
                    cu_limit_set = true;
                    break;
                case CB_IX_SET_CU_PRICE:
                    if (data.size() != 9) return ParseError::UNSUPPORTED_INSTRUCTION;
                    cu_price = read_u64_le(data.data() + 1);
                    break;
                case CB_IX_REQUEST_HEAP_FRAME:
                case CB_IX_SET_DATA_SIZE:
                    if (data.size() != 5) return ParseError::UNSUPPORTED_INSTRUCTION;
                    break;
                default:
                    return ParseError::UNSUPPORTED_INSTRUCTION;
            }
            continue;
        }

        ++non_cb_ix;
        if (have_transfer) return ParseError::MULTIPLE_TRANSFERS;

        if (is_zero_key(prog)) {
            // System Program: Transfer { lamports: u64 }
            if (data.size() != 12 || read_u32_le(data.data()) != SYSTEM_IX_TRANSFER ||
                n_acc != 2) {
                return ParseError::UNSUPPORTED_INSTRUCTION;
            }
            if (acc[0] != 0 || !is_writable(acc[1])) return ParseError::UNSUPPORTED_INSTRUCTION;

            out.asset    = Asset::SOL;
            out.decimals = 9;
            out.amount   = read_u64_le(data.data() + 4);
            std::copy_n(key_at(acc[1]).begin(), 32, out.destination.begin());
            have_transfer = true;
        } else if (key_eq(prog, k.token_program)) {
            // SPL Token: TransferChecked { amount: u64, decimals: u8 }
            // accounts: [source, mint, destination, owner]
            if (data.size() != 10 || data[0] != TOKEN_IX_TRANSFER_CHECKED || n_acc != 4) {
                return ParseError::UNSUPPORTED_INSTRUCTION;
            }
            if (acc[3] != 0 || !is_writable(acc[0]) || !is_writable(acc[2])) {
                return ParseError::UNSUPPORTED_INSTRUCTION;
            }
            if (key_eq(acc[1], k.usdc_mainnet)) {
                out.mint_is_mainnet = true;
            } else if (key_eq(acc[1], k.usdc_devnet)) {
                out.mint_is_mainnet = false;
            } else {
                return ParseError::UNKNOWN_MINT;
            }
            if (data[9] != 6) return ParseError::UNKNOWN_MINT;

            out.asset    = Asset::USDC;
            out.decimals = 6;
            out.amount   = read_u64_le(data.data() + 1);
            std::copy_n(key_at(acc[2]).begin(), 32, out.destination.begin());
            have_transfer = true;
        } else {
            return ParseError::UNSUPPORTED_PROGRAM;
        }
    }

    if (!r.at_end()) return ParseError::TRAILING_BYTES;
    if (!have_transfer) return ParseError::NO_TRANSFER;

    // ── Fee: signatures + ceil(cu_price * cu_limit / 1e6) ─
    if (!cu_limit_set) {
        uint64_t def = static_cast<uint64_t>(DEFAULT_CU_PER_INSTRUCTION) * non_cb_ix;
        cu_limit = static_cast<uint32_t>(std::min<uint64_t>(def, MAX_CU_LIMIT));
    }
    cu_limit = std::min(cu_limit, MAX_CU_LIMIT);
    uint64_t priority = 0;
    if (cu_price > 0 && cu_limit > 0) {
        if (cu_price > std::numeric_limits<uint64_t>::max() / cu_limit) {
            return ParseError::FEE_OVERFLOW;
        }
        priority = (cu_price * cu_limit + 999999) / 1000000;
    }
    out.fee_lamports = LAMPORTS_PER_SIGNATURE * num_sigs + priority;
    return ParseError::OK;
}

const char* error_to_string(ParseError err) {
    switch (err) {
        case ParseError::OK:                      return "ok";
        case ParseError::TRUNCATED:               return "truncated message";
        case ParseError::VERSIONED_UNSUPPORTED:   return "versioned tx unsupported";
        case ParseError::BAD_HEADER:              return "bad header / signer count";
        case ParseError::FEE_PAYER_MISMATCH:      return "fee payer is not this wallet";
        case ParseError::BAD_ACCOUNT_INDEX:       return "bad account index";
        case ParseError::UNSUPPORTED_PROGRAM:     return "unsupported program";
        case ParseError::UNSUPPORTED_INSTRUCTION: return "unsupported instruction";
        case ParseError::NO_TRANSFER:             return "no transfer found";
        case ParseError::MULTIPLE_TRANSFERS:      return "multiple transfers";
        case ParseError::UNKNOWN_MINT:            return "unknown token mint";
        case ParseError::FEE_OVERFLOW:            return "fee overflow";
        case ParseError::TRAILING_BYTES:          return "trailing bytes";
    }
    return "unknown";
}

void format_units(uint64_t amount, uint8_t decimals, char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (decimals > 19) decimals = 19;  // 10^19 is the largest power that fits u64
    uint64_t scale = 1;
    for (uint8_t i = 0; i < decimals; ++i) scale *= 10;

    uint64_t whole = amount / scale;
    uint64_t frac  = amount % scale;
    if (frac == 0) {
        snprintf(out, out_len, "%llu", static_cast<unsigned long long>(whole));
        return;
    }

    // Fixed-width fraction digits (leading zeros kept), trailing zeros trimmed.
    char frac_buf[20];
    for (int i = decimals - 1; i >= 0; --i) {
        frac_buf[i] = static_cast<char>('0' + frac % 10);
        frac /= 10;
    }
    size_t n = decimals;
    while (n > 0 && frac_buf[n - 1] == '0') --n;
    frac_buf[n] = '\0';
    snprintf(out, out_len, "%llu.%s", static_cast<unsigned long long>(whole), frac_buf);
}

} // namespace TxParser
} // namespace Fuchey
