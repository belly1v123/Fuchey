// ============================================================
// Fuchey — Ed25519.cpp
// Thin wrapper around ESP-IDF's mbedTLS PSA Ed25519 support.
// ============================================================

#include "Ed25519.hpp"
#include "esp_log.h"
#include <psa/crypto.h>
#include <algorithm>
#include <cstring>

namespace Fuchey {
namespace Crypto {

static constexpr const char* TAG = "Ed25519";

static bool ensure_psa_ready() {
    static bool initialized = false;
    if (initialized) return true;

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %ld", static_cast<long>(status));
        return false;
    }

    initialized = true;
    return true;
}

static bool import_key_pair(std::span<const uint8_t, 32> priv_key,
                            psa_key_usage_t usage,
                            psa_key_id_t& key_id) {
    if (!ensure_psa_ready()) return false;

    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_TWISTED_EDWARDS));
    psa_set_key_bits(&attrs, 255);
    psa_set_key_usage_flags(&attrs, usage);
    psa_set_key_algorithm(&attrs, PSA_ALG_PURE_EDDSA);

    psa_status_t status = psa_import_key(&attrs,
                                         priv_key.data(),
                                         priv_key.size(),
                                         &key_id);
    psa_reset_key_attributes(&attrs);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key(keypair) failed: %ld", static_cast<long>(status));
        return false;
    }

    return true;
}

static bool import_public_key(std::span<const uint8_t, 32> pub_key,
                              psa_key_id_t& key_id) {
    if (!ensure_psa_ready()) return false;

    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_TWISTED_EDWARDS));
    psa_set_key_bits(&attrs, 255);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attrs, PSA_ALG_PURE_EDDSA);

    psa_status_t status = psa_import_key(&attrs,
                                         pub_key.data(),
                                         pub_key.size(),
                                         &key_id);
    psa_reset_key_attributes(&attrs);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key(pubkey) failed: %ld", static_cast<long>(status));
        return false;
    }

    return true;
}

// ─── Public API ───────────────────────────────────────────────

bool Ed25519::get_pubkey(std::span<const uint8_t, 32> priv_key,
                         std::span<uint8_t, 32>       out_pubkey) {
    psa_key_id_t key_id = 0;
    if (!import_key_pair(priv_key, PSA_KEY_USAGE_EXPORT, key_id)) return false;

    uint8_t exported[32]{};
    size_t exported_len = 0;
    psa_status_t status = psa_export_public_key(key_id,
                                                exported,
                                                sizeof(exported),
                                                &exported_len);
    psa_destroy_key(key_id);

    if (status != PSA_SUCCESS || exported_len != out_pubkey.size()) {
        ESP_LOGE(TAG, "psa_export_public_key failed: status=%ld len=%zu",
                 static_cast<long>(status), exported_len);
        return false;
    }

    std::copy(exported, exported + exported_len, out_pubkey.begin());
    std::memset(exported, 0, sizeof(exported));
    return true;
}

bool Ed25519::sign(std::span<const uint8_t, 32> priv_key,
                   std::span<const uint8_t, 32> pub_key,
                   std::span<const uint8_t>     message,
                   std::span<uint8_t, 64>       out_sig) {
    uint8_t derived_pubkey[32]{};
    if (!get_pubkey(priv_key, std::span<uint8_t, 32>(derived_pubkey, 32))) {
        return false;
    }

    if (std::memcmp(derived_pubkey, pub_key.data(), sizeof(derived_pubkey)) != 0) {
        ESP_LOGE(TAG, "Refusing to sign: public key does not match private key");
        return false;
    }

    psa_key_id_t key_id = 0;
    if (!import_key_pair(priv_key, PSA_KEY_USAGE_SIGN_MESSAGE, key_id)) return false;

    size_t sig_len = 0;
    psa_status_t status = psa_sign_message(key_id,
                                           PSA_ALG_PURE_EDDSA,
                                           message.data(),
                                           message.size(),
                                           out_sig.data(),
                                           out_sig.size(),
                                           &sig_len);
    psa_destroy_key(key_id);

    if (status != PSA_SUCCESS || sig_len != out_sig.size()) {
        ESP_LOGE(TAG, "psa_sign_message failed: status=%ld len=%zu",
                 static_cast<long>(status), sig_len);
        return false;
    }

    return verify(pub_key, message, out_sig);
}

bool Ed25519::verify(std::span<const uint8_t, 32> pub_key,
                     std::span<const uint8_t>     message,
                     std::span<const uint8_t, 64> signature) {
    psa_key_id_t key_id = 0;
    if (!import_public_key(pub_key, key_id)) return false;

    psa_status_t status = psa_verify_message(key_id,
                                             PSA_ALG_PURE_EDDSA,
                                             message.data(),
                                             message.size(),
                                             signature.data(),
                                             signature.size());
    psa_destroy_key(key_id);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_verify_message failed: %ld", static_cast<long>(status));
        return false;
    }

    return true;
}

} // namespace Crypto
} // namespace Fuchey
