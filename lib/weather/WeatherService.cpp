// ============================================================
// Fuchey — WeatherService.cpp
// ============================================================

#include "WeatherService.hpp"
#include "../config/Config.hpp"
#include "esp_log.h"
#include "cJSON.h"
#include <cstdio>

namespace Fuchey {

static constexpr const char* TAG = "WeatherService";

WeatherService::WeatherService(WiFiManager& wifi) : m_wifi(wifi) {}

bool WeatherService::init() {
    ESP_LOGI(TAG, "WeatherService initialized");
    return true;
}

bool WeatherService::parse_weather_json(const std::string& json_str, Events::Event& evt) {
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) return false;

    cJSON* current = cJSON_GetObjectItem(root, "current_weather");
    if (!current) {
        cJSON_Delete(root);
        return false;
    }

    cJSON* temp = cJSON_GetObjectItem(current, "temperature");
    cJSON* wind = cJSON_GetObjectItem(current, "windspeed");
    cJSON* code = cJSON_GetObjectItem(current, "weathercode");

    if (temp) evt.data.weather.temp_celsius = static_cast<float>(temp->valuedouble);
    if (wind) evt.data.weather.wind_speed_kmh = static_cast<float>(wind->valuedouble);
    if (code) evt.data.weather.weather_code = static_cast<uint8_t>(code->valueint);

    snprintf(evt.data.weather.city, sizeof(evt.data.weather.city), "New York");

    cJSON_Delete(root);
    return true;
}

bool WeatherService::update_now() {
    if (!m_wifi.has_ip()) return false;

    char url[256];
    snprintf(url, sizeof(url), API::WEATHER_URL_FMT, m_lat, m_lon);

    auto resp = m_wifi.get(url);
    if (!resp.success) {
        ESP_LOGE(TAG, "Weather HTTP request failed");
        return false;
    }

    Events::Event evt{};
    evt.type = Events::EventType::WEATHER_UPDATED;

    if (parse_weather_json(resp.body, evt)) {
        ESP_LOGI(TAG, "Weather updated: %.1f °C, Code: %d",
                 evt.data.weather.temp_celsius, evt.data.weather.weather_code);
        Events::post(Events::g_ui_queue, evt);
        if (Events::g_event_group) {
            xEventGroupSetBits(Events::g_event_group, Events::BIT_WEATHER_OK);
        }
        return true;
    }

    return false;
}

void WeatherService::task_entry(void* arg) {
    static_cast<WeatherService*>(arg)->run();
}

void WeatherService::run() {
    ESP_LOGI(TAG, "WeatherService task running");
    while (true) {
        if (m_wifi.has_ip()) {
            update_now();
        }
        vTaskDelay(pdMS_TO_TICKS(Timing::WEATHER_UPDATE_MS));
    }
}

} // namespace Fuchey
