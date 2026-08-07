#pragma once
// ============================================================
// Fuchey — WalletCore.hpp
// The security kernel of Fuchey.
//
// SECURITY CONTRACT:
//   - Never exposes private keys outside this module
//     (single deliberate exception: export_secret(), opt-in only)
//   - Never depends on AI, WiFi, or UI
//   - Private key is held in memory only during active session
//   - Mnemonic is stored encrypted in NVS
//   - All key material is zeroed when session ends
//
// DEPENDENCY RULE: WalletCore → Crypto, Storage ONLY
// ============================================================

#include "../crypto/CryptoEngine.hpp"
#include "../storage/Storage.hpp"
#include <string>
#include <optional>
#include <array>
#include <span>
#include <cstdint>

namespace Fuchey {

// ─── Wallet state machine ─────────────────────────────────
enum class WalletState {
    UNINITIALIZED,  // No wallet exists
    LOCKED,         // Wallet exists, session inactive
    UNLOCKED,       // Wallet active, key in memory
};

// ─── Result type for wallet operations ────────────────────
enum class WalletResult {
    OK,
    ERR_ALREADY_EXISTS,
    ERR_INVALID_MNEMONIC,
    ERR_INVALID_PRIVKEY,      // Bad hex, wrong length, or invalid key
    ERR_STORAGE_FAILURE,
    ERR_CRYPTO_FAILURE,
    ERR_LOCKED,
    ERR_NOT_INITIALIZED,
    ERR_SIGNING_FAILED,
};

class WalletCore {
public:
    WalletCore();
    ~WalletCore();

    // Non-copyable — contains sensitive key material
    WalletCore(const WalletCore&) = delete;
    WalletCore& operator=(const WalletCore&) = delete;

    // ── Lifecycle ────────────────────────────────────────
    // Initialize: loads wallet state from NVS
    // Call once at boot.
    WalletResult init();

    // ── Wallet creation ──────────────────────────────────
    // Generate new wallet. Words: 12 or 24.
    // Returns mnemonic string for display — caller must zero it after showing.
    // On success, wallet is UNLOCKED.
    WalletResult create(int words, std::string& out_mnemonic);

    // Import wallet from BIP39 mnemonic (12 or 24 words).
    // On success, wallet is UNLOCKED.
    WalletResult import(std::string_view mnemonic);

    // Import wallet from raw 64-char hex-encoded Ed25519 private key/seed (32 bytes).
    // Derives public key directly — no BIP39/SLIP0010 derivation path.
    // Example: "2f97510b0d6d19dd..." (exactly 64 lowercase hex chars)
    // On success, wallet is UNLOCKED.
    WalletResult import_privkey_hex(std::string_view hex64);

    // Import wallet from a Solana base58 secret key.
    // Accepts 32-byte seed or 64-byte seed+pubkey exports.
    // On success, wallet is UNLOCKED.
    WalletResult import_privkey_base58(std::string_view encoded);

    // ── Session management ───────────────────────────────
    // Lock: clear private key from memory
    void lock();

    // Unlock: reload key from NVS (currently uses in-memory key)
    // For MVP: key stays in memory during session.
    // Future: require PIN to re-derive.
    WalletResult unlock();

    // ── Queries ──────────────────────────────────────────
    WalletState state() const { return m_state; }
    bool is_unlocked() const  { return m_state == WalletState::UNLOCKED; }
    bool has_wallet() const   { return m_state != WalletState::UNINITIALIZED; }

    // Get the public address (Base58) — available when locked or unlocked
    std::optional<std::string> get_address() const;

    // Get public key bytes — available when locked or unlocked
    std::optional<Crypto::PubKey> get_pubkey() const;

    // ── Deliberate secret export (opt-in) ─────────────────────
    // SECURITY: The ONLY method that exposes key material outside this module.
    // Returns the standard Solana 64-byte secret key (priv32 || pub32).
    // Only available when UNLOCKED. Caller MUST zero the returned array after use.
    std::optional<Crypto::SecretKey> export_secret() const;

    // ── Transaction signing ───────────────────────────────
    // SECURITY: Only callable when UNLOCKED.
    // message: raw transaction bytes to sign (NOT the hash — Ed25519 hashes internally)
    // out_sig: 64-byte signature output
    // Returns ERR_LOCKED if wallet is locked.
    WalletResult sign(std::span<const uint8_t> message,
                      Crypto::Signature&        out_sig);

    // ── Factory reset ─────────────────────────────────────
    // Erases all wallet data from NVS. Irreversible.
    WalletResult factory_reset();

private:
    WalletState     m_state{WalletState::UNINITIALIZED};
    Crypto::KeyPair m_keypair;    // Only valid when UNLOCKED
    Crypto::PubKey  m_pubkey{};  // Cached when wallet exists
    bool            m_pubkey_valid{false};

    // ── Helpers ───────────────────────────────────────────
    WalletResult derive_and_store(const Crypto::Seed& seed);
    WalletResult load_pubkey();
    void         zero_sensitive_data();

    static constexpr const char* TAG = "WalletCore";
};

} // namespace Fuchey
