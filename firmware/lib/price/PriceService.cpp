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

PriceService::PriceService(WiFiManager& wifi) : m_wifi(wifi), m_sol_usd(DEFAULT_SOL_USD) {}

bool PriceService::init() {
    ESP_LOGI(TAG, "PriceService initialized");
    return true;
}

bool PriceService::parse_price_json(const std::string& json_str, float& out_price) {
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) return false;

    // Binance format: {"symbol":"SOLUSDT","price":"185.43"}
    // Price is a JSON string, not a number
    cJSON* price = cJSON_GetObjectItem(root, "price");
    if (!price) {
        cJSON_Delete(root);
        return false;
    }

    if (cJSON_IsString(price) && price->valuestring) {
        out_price = static_cast<float>(atof(price->valuestring));
    } else if (cJSON_IsNumber(price)) {
        out_price = static_cast<float>(price->valuedouble);
    } else {
        cJSON_Delete(root);
        return false;
    }

    cJSON_Delete(root);
    return true;
}

bool PriceService::update_now() {
    if (!m_wifi.has_ip()) return false;

    auto resp = m_wifi.get(API::SOL_PRICE_URL);
    if (!resp.success) {
        ESP_LOGE(TAG, "SOL Price HTTP request failed");
        return false;
    }

    float sol_usd = 0.0f;
    if (parse_price_json(resp.body, sol_usd)) {
        ESP_LOGI(TAG, "SOL Price updated: $%.2f", sol_usd);
        m_sol_usd.store(sol_usd);

        Events::Event evt{};
        evt.type = Events::EventType::PRICE_UPDATED;
        evt.data.price.sol_usd = sol_usd;

        Events::post(Events::g_ui_queue, evt);
        if (Events::g_event_group) {
            xEventGroupSetBits(Events::g_event_group, Events::BIT_PRICE_OK);
        }
        return true;
    }

    return false;
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
            bits = xEventGroupWaitBits(Events::g_event_group, Events::BIT_WIFI_IP,
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
