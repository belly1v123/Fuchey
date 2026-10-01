#pragma once
// ============================================================
// Fuchey — TxParser.hpp
// Minimal Solana legacy-message parser used by WalletManager to
// derive what the user is shown from the exact bytes being signed.
//
// Accepted shape (anything else is rejected — fail closed):
//   - legacy message (versioned v0 is rejected), exactly 1 signer,
//     fee payer == this wallet
//   - optional ComputeBudget instructions (limit / price / heap / data size)
//   - exactly ONE transfer:
//       System Program Transfer               → SOL
//       SPL Token TransferChecked, USDC mint  → USDC
//   - USDC only: optionally ONE Associated Token Account CreateIdempotent
//     right before the transfer, paid by this wallet, creating exactly the
//     destination account, which must be the ATA derived from
//     (recipient owner, USDC mint) — verified on device.
// ============================================================

#include "../crypto/CryptoEngine.hpp"
#include <cstdint>
#include <cstddef>
#include <span>

namespace Fuchey {
namespace TxParser {

enum class Asset : uint8_t { SOL, USDC };

enum class ParseError : uint8_t {
    OK,
    TRUNCATED,
    VERSIONED_UNSUPPORTED,
    BAD_HEADER,
    FEE_PAYER_MISMATCH,
    BAD_ACCOUNT_INDEX,
    UNSUPPORTED_PROGRAM,
    UNSUPPORTED_INSTRUCTION,
    NO_TRANSFER,
    MULTIPLE_TRANSFERS,
    UNKNOWN_MINT,
    FEE_OVERFLOW,
    TRAILING_BYTES,
    BAD_TOKEN_ACCOUNT,   // ATA create does not match the transfer / derivation
};

struct ParsedTransfer {
    Asset          asset{Asset::SOL};
    uint8_t        decimals{9};
    uint64_t       amount{0};        // base units (lamports / USDC micro-units)
    Crypto::PubKey destination{};    // SOL: recipient wallet, USDC: dest token account
    bool           mint_is_mainnet{false}; // USDC only: which USDC mint was used
    uint64_t       fee_lamports{0};  // base signature fee + compute-unit priority fee
    // USDC with an ATA create: the recipient wallet (owner of `destination`),
    // and the rent this wallet pays to open that account.
    bool           creates_token_account{false};
    Crypto::PubKey owner{};
    uint64_t       rent_lamports{0};
};

// Parse `message` (the bytes that get signed, WITHOUT the signature
// prefix). `signer` must be the wallet's public key.
ParseError parse_transfer(std::span<const uint8_t> message,
                          const Crypto::PubKey& signer,
                          ParsedTransfer& out);

const char* error_to_string(ParseError err);

// Associated token account of (owner, mint) under the SPL Token program.
bool derive_ata(const Crypto::PubKey& owner, const Crypto::PubKey& mint, Crypto::PubKey& out);

// Exact decimal rendering of an integer amount: (1234500, 6) -> "1.2345".
void format_units(uint64_t amount, uint8_t decimals, char* out, size_t out_len);

} // namespace TxParser
} // namespace Fuchey
