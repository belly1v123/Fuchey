// ============================================================
// Fuchey — BIP39.cpp
// BIP39 mnemonic implementation.
// Uses mbedTLS for PBKDF2-HMAC-SHA512.
// Uses ESP-IDF esp_random() for hardware entropy.
// ============================================================

#include "BIP39.hpp"
#include "SHA256.hpp"
#include "esp_log.h"
#include "esp_random.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace Fuchey {
namespace Crypto {

static constexpr const char* TAG = "BIP39";

// wordlist is defined in wordlist.h
const char* const BIP39::WORDLIST[WORDLIST_SIZE] = {
#include "wordlist.h"
};

// ─── Word list helpers ────────────────────────────────────
const char* BIP39::get_word(int index) {
    if (index < 0 || index >= WORDLIST_SIZE) return nullptr;
    return WORDLIST[index];
}

int BIP39::find_word(std::string_view word) {
    for (int i = 0; i < WORDLIST_SIZE; ++i) {
        if (word == WORDLIST[i]) return i;
    }
    return -1;
}

// ─── Split mnemonic ───────────────────────────────────────
std::vector<std::string_view> BIP39::split_mnemonic(std::string_view mnemonic) {
    std::vector<std::string_view> words;
    size_t start = 0;
    while (start < mnemonic.size()) {
        while (start < mnemonic.size() &&
               !std::isalpha(static_cast<unsigned char>(mnemonic[start]))) {
            ++start;
        }
        if (start >= mnemonic.size()) break;

        size_t end = start;
        while (end < mnemonic.size() &&
               std::isalpha(static_cast<unsigned char>(mnemonic[end]))) {
            ++end;
        }
        if (end > start) words.push_back(mnemonic.substr(start, end - start));
        start = end;
    }
    return words;
}

static std::string normalize_mnemonic(std::string_view mnemonic) {
    std::string normalized;
    size_t start = 0;
    while (start < mnemonic.size()) {
        while (start < mnemonic.size() &&
               !std::isalpha(static_cast<unsigned char>(mnemonic[start]))) {
            ++start;
        }
        if (start >= mnemonic.size()) break;

        if (!normalized.empty()) normalized += ' ';
        while (start < mnemonic.size() &&
               std::isalpha(static_cast<unsigned char>(mnemonic[start]))) {
            normalized += static_cast<char>(std::tolower(static_cast<unsigned char>(mnemonic[start])));
            ++start;
        }
    }
    return normalized;
}

#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_system.h"

std::optional<std::string> BIP39::generate(int words) {
    if (words != 12 && words != 24) {
        ESP_LOGE(TAG, "words must be 12 or 24");
        return std::nullopt;
    }
    int entropy_bytes = (words == 12) ? 16 : 32;
    std::vector<uint8_t> raw_buf(64);

    // 1. ESP32 hardware RNG
    esp_fill_random(raw_buf.data(), 32);

    // 2. High-precision system timers and CPU cycle counters
    int64_t t = esp_timer_get_time();
    uint32_t cycles = esp_cpu_get_cycle_count();
    uint32_t free_heap = esp_get_free_heap_size();

    std::memcpy(raw_buf.data() + 32, &t, sizeof(t));
    std::memcpy(raw_buf.data() + 40, &cycles, sizeof(cycles));
    std::memcpy(raw_buf.data() + 44, &free_heap, sizeof(free_heap));

    // 3. Additional hardware random bytes
    uint32_t extra_rnd = esp_random();
    std::memcpy(raw_buf.data() + 48, &extra_rnd, sizeof(extra_rnd));

    // 4. Hash raw entropy buffer with SHA256 to get cryptographically uniform entropy
    auto hash = sha256(raw_buf);

    std::vector<uint8_t> entropy(entropy_bytes);
    std::copy(hash.begin(), hash.begin() + entropy_bytes, entropy.begin());

    auto result = entropy_to_mnemonic(std::span<const uint8_t>(entropy));

    // Zero sensitive memory immediately
    std::fill(raw_buf.begin(), raw_buf.end(), 0);
    std::fill(entropy.begin(), entropy.end(), 0);

    return result;
}

std::optional<std::string> BIP39::from_entropy(std::span<const uint8_t> entropy) {
    if (entropy.size() != 16 && entropy.size() != 32) {
        ESP_LOGE(TAG, "entropy must be 16 or 32 bytes");
        return std::nullopt;
    }
    return entropy_to_mnemonic(entropy);
}

// ─── Entropy → Mnemonic ───────────────────────────────────
// BIP39 algorithm:
//   1. SHA256(entropy) → checksum
//   2. Append CS bits to entropy bits
//   3. Split into 11-bit groups → word indices
std::optional<std::string> BIP39::entropy_to_mnemonic(std::span<const uint8_t> entropy) {
    const size_t ENT = entropy.size() * 8;  // bits
    const size_t CS  = ENT / 32;             // checksum bits
    const size_t MS  = (ENT + CS) / 11;     // number of words

    // Compute checksum
    auto hash = sha256(entropy);
    uint8_t checksum_byte = hash[0];

    // Build bit stream: entropy bits + CS checksum bits
    // Max: 256 + 8 = 264 bits = 33 bytes
    std::array<uint8_t, 33> bits{};
    std::copy(entropy.begin(), entropy.end(), bits.begin());

    // Append checksum bits (top CS bits of SHA256)
    uint8_t mask = static_cast<uint8_t>(0xFF << (8 - CS));
    bits[entropy.size()] = checksum_byte & mask;

    // Extract 11-bit word indices
    std::string mnemonic;
    for (size_t i = 0; i < MS; ++i) {
        // Find the 11 bits starting at bit offset i*11
        size_t bit_offset = i * 11;
        size_t byte_offset = bit_offset / 8;
        size_t bit_shift   = bit_offset % 8;

        uint32_t val = 0;
        val |= (static_cast<uint32_t>(bits[byte_offset])     << 16);
        val |= (static_cast<uint32_t>(bits[byte_offset + 1]) <<  8);
        val |= (static_cast<uint32_t>(bits[byte_offset + 2])      );
        val >>= (13 - bit_shift);  // right-shift to get 11 bits
        val &= 0x7FF;              // mask to 11 bits

        if (!mnemonic.empty()) mnemonic += ' ';
        mnemonic += WORDLIST[val];
    }

    return mnemonic;
}

// ─── Mnemonic → Entropy (with checksum validation) ────────
std::vector<uint8_t> BIP39::mnemonic_to_entropy(std::string_view mnemonic) {
    std::string normalized_mnemonic = normalize_mnemonic(mnemonic);
    auto words = split_mnemonic(normalized_mnemonic);
    if (words.size() != 12 && words.size() != 24) {
        ESP_LOGE(TAG, "Invalid word count: %zu", words.size());
        return {};
    }

    // Convert words to 11-bit indices
    std::vector<int> indices;
    indices.reserve(words.size());
    for (auto& w : words) {
        int idx = find_word(w);
        if (idx < 0) {
            ESP_LOGE(TAG, "Unknown word in mnemonic");
            return {};
        }
        indices.push_back(idx);
    }

    // Pack indices into bit stream
    size_t total_bits = words.size() * 11;
    std::vector<uint8_t> bit_buf((total_bits + 7) / 8, 0);

    for (size_t i = 0; i < indices.size(); ++i) {
        size_t bit_offset = i * 11;
        size_t byte_off = bit_offset / 8;
        int    bit_shift = static_cast<int>(bit_offset % 8);

        uint32_t val = static_cast<uint32_t>(indices[i]) << (21 - bit_shift);
        bit_buf[byte_off]     |= (val >> 16) & 0xFF;
        bit_buf[byte_off + 1] |= (val >>  8) & 0xFF;
        if (byte_off + 2 < bit_buf.size())
            bit_buf[byte_off + 2] |= val & 0xFF;
    }

    // Split entropy and checksum
    size_t ENT_BITS  = (words.size() == 12) ? 128 : 256;
    size_t ENT_BYTES = ENT_BITS / 8;
    size_t CS_BITS   = ENT_BITS / 32;

    std::vector<uint8_t> entropy(bit_buf.begin(), bit_buf.begin() + ENT_BYTES);

    // Verify checksum
    auto hash = sha256(std::span<const uint8_t>(entropy));
    uint8_t expected_cs = hash[0] >> (8 - CS_BITS);
    uint8_t actual_cs   = bit_buf[ENT_BYTES] >> (8 - CS_BITS);

    if (expected_cs != actual_cs) {
        ESP_LOGE(TAG, "BIP39 checksum mismatch");
        return {};
    }

    return entropy;
}

// ─── Validate ─────────────────────────────────────────────
bool BIP39::validate(std::string_view mnemonic) {
    auto entropy = mnemonic_to_entropy(mnemonic);
    return !entropy.empty();
}

// ─── Mnemonic → Seed (PBKDF2-HMAC-SHA512) ────────────────
bool BIP39::to_seed(std::string_view mnemonic,
                    std::string_view passphrase,
                    std::array<uint8_t, 64>& out_seed) {
    std::string normalized_mnemonic = normalize_mnemonic(mnemonic);

    // Salt = "mnemonic" + passphrase
    std::string salt_str = "mnemonic";
    salt_str += passphrase;

    auto mnemonic_bytes = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(normalized_mnemonic.data()), normalized_mnemonic.size());
    auto salt_bytes = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(salt_str.data()), salt_str.size());

    bool ok = pbkdf2_hmac_sha512(
        mnemonic_bytes, salt_bytes,
        2048,  // BIP39 iterations
        std::span<uint8_t>(out_seed.data(), out_seed.size())
    );

    // Zero salt
    std::fill(salt_str.begin(), salt_str.end(), '\0');
    std::fill(normalized_mnemonic.begin(), normalized_mnemonic.end(), '\0');

    if (!ok) {
        ESP_LOGE(TAG, "PBKDF2 failed");
        out_seed.fill(0);
    }
    return ok;
}

} // namespace Crypto
} // namespace Fuchey
