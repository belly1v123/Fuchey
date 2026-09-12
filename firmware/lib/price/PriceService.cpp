// ============================================================
// Fuchey — PriceService.cpp
// ============================================================

#include "PriceService.hpp"
#include "../config/Config.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

namespace Fuchey {

static constexpr const char* TAG = "PriceService";

PriceService::PriceService(WiFiManager& wifi) : m_wifi(wifi), m_sol_usd(-1.0f) {}

bool PriceService::init() {
    ESP_LOGI(TAG, "PriceService initialized");
    return true;
}

// Parse a numeric field that may be a JSON string ("185.43") or number.
static bool json_num_to_float(cJSON* obj, const char* key, float& out) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (!item) return false;
    if (cJSON_IsString(item) && item->valuestring) {
        out = static_cast<float>(atof(item->valuestring));
        return true;
    }
    if (cJSON_IsNumber(item)) {
        out = static_cast<float>(item->valuedouble);
        return true;
    }
    return false;
}

bool PriceService::parse_price_json(const std::string& json_str, float& out_price,
                                    float& out_high, float& out_low,
                                    float& out_change_pct) {
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) return false;

    // 24hr ticker: {"lastPrice":"185.43","highPrice":"...","lowPrice":"...","priceChangePercent":"2.36",...}
    // Legacy spot format {"symbol":"SOLUSDT","price":"185.43"} still accepted.
    bool ok = false;
    if (json_num_to_float(root, "lastPrice", out_price)) {
        // Full 24h context; individual fields optional so a partial
        // response still yields a usable price.
        json_num_to_float(root, "highPrice", out_high);
        json_num_to_float(root, "lowPrice", out_low);
        json_num_to_float(root, "priceChangePercent", out_change_pct);
        ok = true;
    } else if (json_num_to_float(root, "price", out_price)) {
        out_high = -1.0f;
        out_low = -1.0f;
        out_change_pct = 0.0f;
        ok = true;
    }

    cJSON_Delete(root);
    return ok;
}

bool PriceService::update_now() {
    if (!m_wifi.has_ip()) return false;

    auto resp = m_wifi.get(API::SOL_PRICE_24H_URL);
    if (!resp.success) {
        ESP_LOGE(TAG, "SOL Price HTTP request failed");
        return false;
    }

    float sol_usd = 0.0f, high = -1.0f, low = -1.0f, change = 0.0f;
    if (parse_price_json(resp.body, sol_usd, high, low, change)) {
        ESP_LOGI(TAG, "SOL Price updated: $%.2f hi=%.2f lo=%.2f chg=%+.2f%%",
                 sol_usd, high, low, change);
        m_sol_usd.store(sol_usd);
        if (high > 0.0f) m_high_24h.store(high);
        if (low > 0.0f) m_low_24h.store(low);
        m_change_pct.store(change);

        Events::Event evt{};
        evt.type = Events::EventType::PRICE_UPDATED;
        evt.data.price.sol_usd = sol_usd;
        evt.data.price.high_24h = m_high_24h.load();
        evt.data.price.low_24h = m_low_24h.load();
        evt.data.price.change_pct_24h = change;

        Events::post(Events::g_ui_queue, evt);
        if (Events::g_event_group) {
            xEventGroupSetBits(Events::g_event_group, Events::BIT_PRICE_OK);
        }
        return true;
    }

    return false;
}

void PriceService::request_update() {
    if (Events::g_event_group) {
        xEventGroupSetBits(Events::g_event_group, Events::BIT_PRICE_FETCH_REQ);
    }
}

void PriceService::task_entry(void* arg) {
    static_cast<PriceService*>(arg)->run();
}

void PriceService::run() {
    ESP_LOGI(TAG, "PriceService task running");
    int64_t ip_ts_us = 0;
    while (true) {
        uint32_t bits = 0;
        if (Events::g_event_group) {
            bits = xEventGroupWaitBits(Events::g_event_group,
                                       Events::BIT_WIFI_IP | Events::BIT_PRICE_FETCH_REQ,
                                       pdTRUE, pdFALSE,
                                       pdMS_TO_TICKS(Timing::PRICE_UPDATE_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(Timing::PRICE_UPDATE_MS));
        }
        if (!m_wifi.has_ip()) continue;

        if (bits & Events::BIT_WIFI_IP) {
            ip_ts_us = esp_timer_get_time();  // woke on fresh IP signal
        }
        bool ok = update_now();
        if (ip_ts_us != 0) {
            if (ok) {
                ESP_LOGI(TAG, "SOL Price fetched %.0f ms after WiFi IP",
                         static_cast<double>(esp_timer_get_time() - ip_ts_us) / 1000.0);
            }
            ip_ts_us = 0;
        }
    }
}

} // namespace Fuchey
