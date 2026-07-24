#pragma once
// ============================================================
// Fuchey — BIP39.hpp
// BIP39 mnemonic generation, validation, and seed derivation.
// Fully spec-compliant (BIP39 test vectors pass).
//
// Entropy sizes:
//   128 bits → 12 words
//   256 bits → 24 words
// ============================================================

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <optional>
#include <array>
#include <span>

namespace Fuchey {
namespace Crypto {

class BIP39 {
public:
    static constexpr int WORDLIST_SIZE = 2048;

    // ── Generate ─────────────────────────────────────────
    // Generate mnemonic from hardware RNG entropy
    // words must be 12 or 24
    static std::optional<std::string> generate(int words = 24);

    // Generate mnemonic from caller-provided entropy bytes
    // entropy_len must be 16 (12 words) or 32 (24 words)
    static std::optional<std::string> from_entropy(
        std::span<const uint8_t> entropy);

    // ── Validate ─────────────────────────────────────────
    // Returns true if mnemonic is valid (word list + checksum)
    static bool validate(std::string_view mnemonic);

    // ── Seed derivation ───────────────────────────────────
    // BIP39: PBKDF2(mnemonic, "mnemonic" + passphrase, 2048, 64)
    // Returns false on failure.
    // SECURITY: Zero out seed when done with it.
    static bool to_seed(std::string_view mnemonic,
                        std::string_view passphrase,
                        std::array<uint8_t, 64>& out_seed);

    // ── Word list helpers ─────────────────────────────────
    static const char* get_word(int index);
    static int find_word(std::string_view word); // -1 if not found

    // ── Internal helpers (exposed for testing) ────────────
    // Convert entropy bytes → mnemonic string
    static std::optional<std::string> entropy_to_mnemonic(
        std::span<const uint8_t> entropy);

    // Convert mnemonic → entropy bytes (validates checksum)
    // Returns empty vector on failure
    static std::vector<uint8_t> mnemonic_to_entropy(
        std::string_view mnemonic);

private:
    // The 2048-word English BIP39 word list (defined in wordlist.cpp)
    static const char* const WORDLIST[WORDLIST_SIZE];

    // Split mnemonic string into words
    static std::vector<std::string_view> split_mnemonic(
        std::string_view mnemonic);

    static constexpr const char* TAG = "BIP39";
};

} // namespace Crypto
} // namespace Fuchey
