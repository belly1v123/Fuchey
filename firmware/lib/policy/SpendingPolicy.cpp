// ============================================================
// Fuchey — SpendingPolicy.cpp
// ============================================================

#include "SpendingPolicy.hpp"
#include "../storage/Storage.hpp"
#include "../config/Config.hpp"
#include "esp_log.h"

namespace Fuchey {

static constexpr const char* TAG = "SpendingPolicy";

SpendingPolicy::SpendingPolicy() = default;

bool SpendingPolicy::init() {
    Storage::Handle nvs(NVS::POLICY_NS, NVS_READONLY);
    if (!nvs.is_open()) {
        // Default to OFF (most restrictive)
        m_limit = SpendLimit::OFF;
        ESP_LOGI(TAG, "No policy found — defaulting to OFF");
        return true;
    }

    auto stored = nvs.get_u32(NVS::KEY_SPEND_LIMIT);
    if (!stored) {
        m_limit = SpendLimit::OFF;
        ESP_LOGI(TAG, "No stored limit — defaulting to OFF");
        return true;
    }

    // Validate the stored value matches a known preset
    switch (static_cast<SpendLimit>(*stored)) {
        case SpendLimit::OFF:
        case SpendLimit::CENTS_50:
        case SpendLimit::USD_1:
        case SpendLimit::USD_5:
            m_limit = static_cast<SpendLimit>(*stored);
            break;
        default:
            ESP_LOGW(TAG, "Unknown limit value %lu — defaulting to OFF", *stored);
            m_limit = SpendLimit::OFF;
    }

    ESP_LOGI(TAG, "Spending limit loaded: %s", limit_to_string());
    return true;
}

bool SpendingPolicy::set_limit(SpendLimit limit) {
    Storage::Handle nvs(NVS::POLICY_NS, NVS_READWRITE);
    if (!nvs.is_open()) return false;

    if (!nvs.set_u32(NVS::KEY_SPEND_LIMIT, static_cast<uint32_t>(limit))) {
        return false;
    }
    if (!nvs.commit()) return false;

    m_limit = limit;
    ESP_LOGI(TAG, "Spending limit updated to: %s", limit_to_string());
    return true;
}

PolicyDecision SpendingPolicy::evaluate(uint64_t amount_cents) const {
    if (m_limit == SpendLimit::OFF) {
        return PolicyDecision::REQUIRE_CONFIRMATION;
    }

    uint64_t threshold = static_cast<uint64_t>(m_limit);
    if (amount_cents <= threshold) {
        ESP_LOGD(TAG, "Amount %llu cents <= threshold %llu — AUTO_SIGN",
                 amount_cents, threshold);
        return PolicyDecision::AUTO_SIGN;
    }

    ESP_LOGD(TAG, "Amount %llu cents > threshold %llu — REQUIRE_CONFIRMATION",
             amount_cents, threshold);
    return PolicyDecision::REQUIRE_CONFIRMATION;
}

const char* SpendingPolicy::limit_to_string() const {
    return limit_to_string(m_limit);
}

const char* SpendingPolicy::limit_to_string(SpendLimit limit) {
    switch (limit) {
        case SpendLimit::OFF:      return "OFF";
        case SpendLimit::CENTS_50: return "$0.50";
        case SpendLimit::USD_1:    return "$1.00";
        case SpendLimit::USD_5:    return "$5.00";
        default:                    return "Unknown";
    }
}

} // namespace Fuchey
