// ============================================================
// Fuchey — WalletCore.cpp
// ============================================================

#include "WalletCore.hpp"
#include "../crypto/BIP39.hpp"
#include "../crypto/SLIP0010.hpp"
#include "../crypto/Ed25519.hpp"
#include "../crypto/Base58.hpp"
#include "../config/Config.hpp"
#include "esp_log.h"
#include <cstring>
#include <algorithm>

namespace Fuchey {

static constexpr const char* TAG = "WalletCore";

WalletCore::WalletCore() = default;

WalletCore::~WalletCore() {
    zero_sensitive_data();
}

// ─── Init ─────────────────────────────────────────────────
WalletResult WalletCore::init() {
    // Check NVS for existing wallet
    Storage::Handle nvs(NVS::WALLET_NS, NVS_READONLY);
    if (!nvs.is_open()) {
        // NVS not yet initialized — wallet doesn't exist
        m_state = WalletState::UNINITIALIZED;
        ESP_LOGI(TAG, "No wallet found — UNINITIALIZED");
        return WalletResult::OK;
    }

    auto created = nvs.get_u8(NVS::KEY_WALLET_CREATED);
    if (!created || *created != 1) {
        m_state = WalletState::UNINITIALIZED;
        ESP_LOGI(TAG, "Wallet marker not set — UNINITIALIZED");
        return WalletResult::OK;
    }

    // Wallet exists — load pubkey
    auto result = load_pubkey();
    if (result != WalletResult::OK) {
        ESP_LOGE(TAG, "Failed to load pubkey from NVS");
        m_state = WalletState::UNINITIALIZED;
        return result;
    }

    m_state = WalletState::UNLOCKED;
    ESP_LOGI(TAG, "Wallet found — UNLOCKED");
    return WalletResult::OK;
}

// ─── Create ───────────────────────────────────────────────
WalletResult WalletCore::create(int words, std::string& out_mnemonic) {
    if (m_state != WalletState::UNINITIALIZED) {
        ESP_LOGW(TAG, "Wallet already exists");
        return WalletResult::ERR_ALREADY_EXISTS;
    }

    // Generate mnemonic from hardware RNG
    auto mnemonic = Crypto::BIP39::generate(words);
    if (!mnemonic) {
        ESP_LOGE(TAG, "BIP39 mnemonic generation failed");
        return WalletResult::ERR_CRYPTO_FAILURE;
    }

    out_mnemonic = *mnemonic;

    // Derive seed from mnemonic (no passphrase for MVP)
    Crypto::Seed seed{};
    bool ok = Crypto::BIP39::to_seed(*mnemonic, "", seed);
    if (!ok) {
        out_mnemonic.assign(out_mnemonic.size(), '\0'); // zero mnemonic on failure
        out_mnemonic.clear();
        return WalletResult::ERR_CRYPTO_FAILURE;
    }

    // Derive keypair and store pubkey
    auto result = derive_and_store(seed);
    seed.fill(0); // Zero seed immediately

    if (result == WalletResult::OK) {
        m_state = WalletState::UNLOCKED;
        ESP_LOGI(TAG, "Wallet created successfully");
        // NOTE: out_mnemonic is returned to caller for display.
        // Caller MUST zero it after showing.
    }
    return result;
}

// ─── Import ───────────────────────────────────────────────
WalletResult WalletCore::import(std::string_view mnemonic) {
    if (m_state != WalletState::UNINITIALIZED) {
        return WalletResult::ERR_ALREADY_EXISTS;
    }

    // Validate mnemonic first
    if (!Crypto::BIP39::validate(mnemonic)) {
        ESP_LOGE(TAG, "Invalid mnemonic provided");
        return WalletResult::ERR_INVALID_MNEMONIC;
    }

    // Derive seed
    Crypto::Seed seed{};
    bool ok = Crypto::BIP39::to_seed(mnemonic, "", seed);
    if (!ok) {
        return WalletResult::ERR_CRYPTO_FAILURE;
    }

    auto result = derive_and_store(seed);
    seed.fill(0);

    if (result == WalletResult::OK) {
        m_state = WalletState::UNLOCKED;
        ESP_LOGI(TAG, "Wallet imported from mnemonic successfully");
    }
    return result;
}

// ─── Import from raw private key (hex) ───────────────────
WalletResult WalletCore::import_privkey_hex(std::string_view hex64) {
    if (m_state != WalletState::UNINITIALIZED) {
        return WalletResult::ERR_ALREADY_EXISTS;
    }

    // Must be exactly 64 hex chars (32 bytes)
    if (hex64.size() != 64) {
        ESP_LOGE(TAG, "import_privkey_hex: expected 64 hex chars, got %zu", hex64.size());
        return WalletResult::ERR_INVALID_PRIVKEY;
    }

    // Parse hex string to 32-byte private key
    Crypto::PrivKey priv_key{};
    for (size_t i = 0; i < 32; ++i) {
        char hi = hex64[i * 2];
        char lo = hex64[i * 2 + 1];

        auto hex_val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        int h = hex_val(hi);
        int l = hex_val(lo);
        if (h < 0 || l < 0) {
            ESP_LOGE(TAG, "import_privkey_hex: invalid hex character at pos %zu", i * 2);
            priv_key.fill(0);
            return WalletResult::ERR_INVALID_PRIVKEY;
        }
        priv_key[i] = static_cast<uint8_t>((h << 4) | l);
    }

    // Derive public key directly from private key using Ed25519
    Crypto::PubKey pub_key{};
    bool ok = Crypto::Ed25519::get_pubkey(
        std::span<const uint8_t, 32>(priv_key.data(), 32),
        std::span<uint8_t, 32>(pub_key.data(), 32)
    );

    if (!ok) {
        priv_key.fill(0);
        ESP_LOGE(TAG, "import_privkey_hex: Ed25519 pubkey derivation failed");
        return WalletResult::ERR_CRYPTO_FAILURE;
    }

    // Store pubkey and privkey in NVS
    Storage::Handle nvs(NVS::WALLET_NS, NVS_READWRITE);
    if (!nvs.is_open()) {
        priv_key.fill(0);
        return WalletResult::ERR_STORAGE_FAILURE;
    }
    if (!nvs.set_blob("pubkey", pub_key.data(), pub_key.size()) ||
        !nvs.set_blob("privkey", priv_key.data(), priv_key.size())) {
        priv_key.fill(0);
        return WalletResult::ERR_STORAGE_FAILURE;
    }
    if (!nvs.set_u8(NVS::KEY_WALLET_CREATED, 1)) {
        priv_key.fill(0);
        return WalletResult::ERR_STORAGE_FAILURE;
    }
    if (!nvs.commit()) {
        priv_key.fill(0);
        return WalletResult::ERR_STORAGE_FAILURE;
    }

    // Store keypair in memory (chain_code is not applicable — zero it)
    m_keypair.priv_key = priv_key;
    m_keypair.pub_key  = pub_key;
    m_keypair.chain_code.fill(0);
    m_pubkey       = pub_key;
    m_pubkey_valid = true;

    // Zero the local copy of the raw private key
    priv_key.fill(0);

    m_state = WalletState::UNLOCKED;
    ESP_LOGI(TAG, "Wallet imported from raw private key successfully");
    return WalletResult::OK;
}

// ─── Lock ─────────────────────────────────────────────────
void WalletCore::lock() {
    zero_sensitive_data();
    if (m_state == WalletState::UNLOCKED) {
        m_state = WalletState::LOCKED;
        ESP_LOGI(TAG, "Wallet locked");
    }
}

// ─── Unlock ───────────────────────────────────────────────
// MVP: For now, unlock just transitions state (key stays in memory during boot)
// Future: will re-derive from NVS-stored encrypted mnemonic + PIN
WalletResult WalletCore::unlock() {
    if (m_state == WalletState::UNINITIALIZED) {
        return WalletResult::ERR_NOT_INITIALIZED;
    }
    if (m_state == WalletState::UNLOCKED) {
        return WalletResult::OK;
    }
    // TODO: Re-derive keypair from NVS + PIN for post-MVP
    m_state = WalletState::UNLOCKED;
    ESP_LOGI(TAG, "Wallet unlocked");
    return WalletResult::OK;
}

// ─── Get address ──────────────────────────────────────────
std::optional<std::string> WalletCore::get_address() const {
    if (!m_pubkey_valid) return std::nullopt;
    return Crypto::Base58::pubkey_to_address(
        std::span<const uint8_t, 32>(m_pubkey.data(), 32));
}

std::optional<Crypto::PubKey> WalletCore::get_pubkey() const {
    if (!m_pubkey_valid) return std::nullopt;
    return m_pubkey;
}

// ─── Sign ─────────────────────────────────────────────────
WalletResult WalletCore::sign(std::span<const uint8_t> message,
                               Crypto::Signature&        out_sig) {
    if (m_state != WalletState::UNLOCKED) {
        ESP_LOGE(TAG, "Cannot sign: wallet is locked");
        return WalletResult::ERR_LOCKED;
    }

    bool ok = Crypto::Ed25519::sign(
        std::span<const uint8_t, 32>(m_keypair.priv_key.data(), 32),
        std::span<const uint8_t, 32>(m_keypair.pub_key.data(), 32),
        message,
        std::span<uint8_t, 64>(out_sig.data(), 64)
    );

    if (!ok) {
        ESP_LOGE(TAG, "Ed25519 signing failed");
        return WalletResult::ERR_SIGNING_FAILED;
    }
    return WalletResult::OK;
}

// ─── Factory reset ────────────────────────────────────────
WalletResult WalletCore::factory_reset() {
    zero_sensitive_data();

    Storage::Handle nvs(NVS::WALLET_NS, NVS_READWRITE);
    if (!nvs.is_open()) return WalletResult::ERR_STORAGE_FAILURE;

    nvs.erase_key(NVS::KEY_WALLET_CREATED);
    nvs.erase_key("pubkey");
    nvs.erase_key("privkey");
    nvs.commit();


    m_state = WalletState::UNINITIALIZED;
    m_pubkey_valid = false;
    ESP_LOGW(TAG, "Factory reset complete");
    return WalletResult::OK;
}

// ─── Private: derive_and_store ────────────────────────────
WalletResult WalletCore::derive_and_store(const Crypto::Seed& seed) {
    // Derive Solana keypair: m/44'/501'/0'/0'
    auto kp = Crypto::SLIP0010::derive_solana(seed);
    if (!kp) {
        ESP_LOGE(TAG, "SLIP0010 derivation failed");
        return WalletResult::ERR_CRYPTO_FAILURE;
    }

    // Derive public key from private key
    Crypto::PubKey pub{};
    bool ok = Crypto::Ed25519::get_pubkey(
        std::span<const uint8_t, 32>(kp->priv_key.data(), 32),
        std::span<uint8_t, 32>(pub.data(), 32)
    );
    if (!ok) {
        ESP_LOGE(TAG, "Failed to derive public key");
        return WalletResult::ERR_CRYPTO_FAILURE;
    }

    kp->pub_key = pub;

    // Store pubkey & privkey in NVS
    Storage::Handle nvs(NVS::WALLET_NS, NVS_READWRITE);
    if (!nvs.is_open()) return WalletResult::ERR_STORAGE_FAILURE;

    if (!nvs.set_blob("pubkey", pub.data(), pub.size()) ||
        !nvs.set_blob("privkey", kp->priv_key.data(), kp->priv_key.size())) {
        return WalletResult::ERR_STORAGE_FAILURE;
    }
    if (!nvs.set_u8(NVS::KEY_WALLET_CREATED, 1)) {
        return WalletResult::ERR_STORAGE_FAILURE;
    }
    if (!nvs.commit()) {
        return WalletResult::ERR_STORAGE_FAILURE;
    }

    // Store keypair in memory
    m_keypair = std::move(*kp);
    m_pubkey = pub;
    m_pubkey_valid = true;

    ESP_LOGI(TAG, "Keypair derived and pubkey stored");
    return WalletResult::OK;
}

// ─── Private: load_pubkey ─────────────────────────────────
WalletResult WalletCore::load_pubkey() {
    Storage::Handle nvs(NVS::WALLET_NS, NVS_READONLY);
    if (!nvs.is_open()) return WalletResult::ERR_STORAGE_FAILURE;

    size_t len = nvs.get_blob("pubkey", m_pubkey.data(), m_pubkey.size());
    if (len != Crypto::PUBKEY_BYTES) {
        ESP_LOGE(TAG, "Pubkey blob invalid (len=%zu)", len);
        return WalletResult::ERR_STORAGE_FAILURE;
    }

    size_t priv_len = nvs.get_blob("privkey", m_keypair.priv_key.data(), m_keypair.priv_key.size());
    if (priv_len == Crypto::PRIVKEY_BYTES) {
        m_keypair.pub_key = m_pubkey;
        ESP_LOGI(TAG, "Loaded keypair successfully from NVS");
    } else {
        ESP_LOGW(TAG, "Privkey blob missing or invalid (len=%zu)", priv_len);
    }

    m_pubkey_valid = true;
    return WalletResult::OK;
}

// ─── Private: zero_sensitive_data ─────────────────────────
void WalletCore::zero_sensitive_data() {
    m_keypair.priv_key.fill(0);
    m_keypair.chain_code.fill(0);
    // m_pubkey NOT zeroed — it's public
}

} // namespace Fuchey
