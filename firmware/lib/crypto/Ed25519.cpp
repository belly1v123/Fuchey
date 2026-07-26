// ============================================================
// Fuchey — Ed25519.cpp
// Ed25519 wrapper using libsodium, matching Solana secret-key layout.
// ============================================================

#include "Ed25519.hpp"
#include "esp_random.h"
#include "esp_log.h"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <cstring>

namespace Fuchey {
namespace Crypto {

static constexpr const char* TAG = "Ed25519";

static const char* esp_random_name() {
    return "esp_random";
}

static uint32_t esp_random_word() {
    return esp_random();
}

static void esp_random_stir() {
}

static void esp_random_buf(void* const buf, const size_t size) {
    esp_fill_random(buf, size);
}

static int esp_random_close() {
    return 0;
}

static const randombytes_implementation ESP_RANDOM_IMPL = {
    esp_random_name,
    esp_random_word,
    esp_random_stir,
    nullptr,
    esp_random_buf,
    esp_random_close
};

static bool ensure_sodium_ready() {
    static bool initialized = false;
    if (initialized) return true;

    randombytes_set_implementation(&ESP_RANDOM_IMPL);

    if (sodium_init() < 0) {
        ESP_LOGE(TAG, "sodium_init failed");
        return false;
    }

    initialized = true;
    return true;
}

static bool secret_from_seed(std::span<const uint8_t, 32> seed,
                             std::span<uint8_t, 32> out_pubkey,
                             std::span<uint8_t, 64> out_secret) {
    if (!ensure_sodium_ready()) return false;

    int rc = crypto_sign_ed25519_seed_keypair(
        out_pubkey.data(),
        out_secret.data(),
        seed.data()
    );
    if (rc != 0) {
        ESP_LOGE(TAG, "crypto_sign_ed25519_seed_keypair failed: %d", rc);
        return false;
    }
    return true;
}

bool Ed25519::get_pubkey(std::span<const uint8_t, 32> priv_key,
                         std::span<uint8_t, 32> out_pubkey) {
    std::array<uint8_t, 64> secret{};
    bool ok = secret_from_seed(priv_key, out_pubkey,
                               std::span<uint8_t, 64>(secret.data(), secret.size()));
    sodium_memzero(secret.data(), secret.size());
    ESP_LOGI(TAG, "get_pubkey: input privkey");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, priv_key.data(), priv_key.size(), ESP_LOG_INFO);
    ESP_LOGI(TAG, "get_pubkey: output pubkey");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, out_pubkey.data(), out_pubkey.size(), ESP_LOG_INFO);
    return ok;
}

bool Ed25519::sign(std::span<const uint8_t, 32> priv_key,
                   std::span<const uint8_t, 32> pub_key,
                   std::span<const uint8_t> message,
                   std::span<uint8_t, 64> out_sig) {
    std::array<uint8_t, 32> derived_pubkey{};
    std::array<uint8_t, 64> secret{};
    if (!secret_from_seed(priv_key,
                          std::span<uint8_t, 32>(derived_pubkey.data(), derived_pubkey.size()),
                          std::span<uint8_t, 64>(secret.data(), secret.size()))) {
        sodium_memzero(secret.data(), secret.size());
        return false;
    }

    if (!std::equal(derived_pubkey.begin(), derived_pubkey.end(), pub_key.begin())) {
        ESP_LOGE(TAG, "Private/public key mismatch");
        sodium_memzero(derived_pubkey.data(), derived_pubkey.size());
        sodium_memzero(secret.data(), secret.size());
        return false;
    }
    sodium_memzero(derived_pubkey.data(), derived_pubkey.size());

    unsigned long long sig_len = 0;
    const uint8_t empty_message = 0;
    const uint8_t* msg = message.empty() ? &empty_message : message.data();
    int rc = crypto_sign_ed25519_detached(
        out_sig.data(),
        &sig_len,
        msg,
        static_cast<unsigned long long>(message.size()),
        secret.data()
    );
    sodium_memzero(secret.data(), secret.size());

    if (rc != 0 || sig_len != out_sig.size()) {
        ESP_LOGE(TAG, "crypto_sign_ed25519_detached failed: rc=%d len=%llu", rc, sig_len);
        return false;
    }
    return true;
}

bool Ed25519::verify(std::span<const uint8_t, 32> pub_key,
                     std::span<const uint8_t> message,
                     std::span<const uint8_t, 64> signature) {
    if (!ensure_sodium_ready()) return false;

    const uint8_t empty_message = 0;
    const uint8_t* msg = message.empty() ? &empty_message : message.data();
    int rc = crypto_sign_ed25519_verify_detached(
        signature.data(),
        msg,
        static_cast<unsigned long long>(message.size()),
        pub_key.data()
    );
    return rc == 0;
}

bool Ed25519::self_test() {
    static constexpr std::array<uint8_t, 32> seed = {
        0x9d,0x61,0xb1,0x9d,0xef,0xfd,0x5a,0x60,
        0xba,0x84,0x4a,0xf4,0x92,0xec,0x2c,0xc4,
        0x44,0x49,0xc5,0x69,0x7b,0x32,0x69,0x19,
        0x70,0x3b,0xac,0x03,0x1c,0xae,0x7f,0x60
    };
    static constexpr std::array<uint8_t, 32> expected_pubkey = {
        0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,
        0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,
        0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,
        0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a
    };
    static constexpr std::array<uint8_t, 64> expected_sig = {
        0xe5,0x56,0x43,0x00,0xc3,0x60,0xac,0x72,
        0x90,0x86,0xe2,0xcc,0x80,0x6e,0x82,0x8a,
        0x84,0x87,0x7f,0x1e,0xb8,0xe5,0xd9,0x74,
        0xd8,0x73,0xe0,0x65,0x22,0x49,0x01,0x55,
        0x5f,0xb8,0x82,0x15,0x90,0xa3,0x3b,0xac,
        0xc6,0x1e,0x39,0x70,0x1c,0xf9,0xb4,0x6b,
        0xd2,0x5b,0xf5,0xf0,0x59,0x5b,0xbe,0x24,
        0x65,0x51,0x41,0x43,0x8e,0x7a,0x10,0x0b
    };

    std::array<uint8_t, 32> pubkey{};
    if (!get_pubkey(std::span<const uint8_t, 32>(seed.data(), seed.size()),
                    std::span<uint8_t, 32>(pubkey.data(), pubkey.size())) ||
        pubkey != expected_pubkey) {
        ESP_LOGE(TAG, "Self-test failed: public key vector mismatch");
        return false;
    }

    std::array<uint8_t, 64> sig{};
    std::span<const uint8_t> empty_msg;
    if (!sign(std::span<const uint8_t, 32>(seed.data(), seed.size()),
              std::span<const uint8_t, 32>(expected_pubkey.data(), expected_pubkey.size()),
              empty_msg,
              std::span<uint8_t, 64>(sig.data(), sig.size())) ||
        sig != expected_sig) {
        ESP_LOGE(TAG, "Self-test failed: signature vector mismatch");
        return false;
    }

    if (!verify(std::span<const uint8_t, 32>(expected_pubkey.data(), expected_pubkey.size()),
                empty_msg,
                std::span<const uint8_t, 64>(sig.data(), sig.size()))) {
        ESP_LOGE(TAG, "Self-test failed: signature verification failed");
        return false;
    }

    ESP_LOGI(TAG, "Self-test passed");
    return true;
}

} // namespace Crypto
} // namespace Fuchey
