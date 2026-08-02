// ============================================================
// Fuchey — WeatherService.cpp
// ============================================================

#include "WeatherService.hpp"
#include "../config/Config.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <cstdio>

namespace Fuchey {

static constexpr const char* TAG = "WeatherService";

WeatherService::WeatherService(WiFiManager& wifi) : m_wifi(wifi) {}

bool WeatherService::init() {
    load_config();
    ESP_LOGI(TAG, "WeatherService initialized (city=%s, lat=%.4f, lon=%.4f)",
             m_city_name.c_str(), m_lat, m_lon);
    return true;
}

void WeatherService::set_location(const char* city, float lat, float lon) {
    m_city_name = city;
    m_lat = lat;
    m_lon = lon;
    ESP_LOGI(TAG, "Location set: %s (%.4f, %.4f)", m_city_name.c_str(), m_lat, m_lon);
}

void WeatherService::load_config() {
    Storage::Handle cfg(NVS::CONFIG_NS, NVS_READONLY);
    if (cfg.is_open()) {
        auto city = cfg.get_str(NVS::KEY_WEATHER_CITY);
        if (city) m_city_name = std::move(*city);

        auto lat_str = cfg.get_str(NVS::KEY_WEATHER_LAT);
        if (lat_str) m_lat = std::stof(*lat_str);

        auto lon_str = cfg.get_str(NVS::KEY_WEATHER_LON);
        if (lon_str) m_lon = std::stof(*lon_str);
    }
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

    snprintf(evt.data.weather.city, sizeof(evt.data.weather.city), "%s", m_city_name.c_str());

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

void WeatherService::geolocate() {
    // ipapi.co: free, HTTPS, no API key required
    auto resp = m_wifi.get("https://ipapi.co/json/");
    if (!resp.success) {
        ESP_LOGW(TAG, "Geolocation request failed, using default: %s", m_city_name.c_str());
        return;
    }

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) {
        ESP_LOGW(TAG, "Geolocation JSON parse failed, using default: %s", m_city_name.c_str());
        return;
    }

    cJSON* city = cJSON_GetObjectItem(root, "city");
    cJSON* lat  = cJSON_GetObjectItem(root, "latitude");   // ipapi.co uses "latitude"
    cJSON* lon  = cJSON_GetObjectItem(root, "longitude");  // ipapi.co uses "longitude"

    if (city && cJSON_IsString(city) && lat && cJSON_IsNumber(lat) && lon && cJSON_IsNumber(lon)) {
        set_location(city->valuestring,
                     static_cast<float>(lat->valuedouble),
                     static_cast<float>(lon->valuedouble));
        ESP_LOGI(TAG, "Geolocated: %s (%.4f, %.4f)",
                 city->valuestring, lat->valuedouble, lon->valuedouble);
    } else {
        ESP_LOGW(TAG, "Geolocation fields missing, using default: %s", m_city_name.c_str());
    }

    cJSON_Delete(root);
}

void WeatherService::run() {
    ESP_LOGI(TAG, "WeatherService task running");
    int64_t ip_ts_us = 0;
    while (true) {
        uint32_t bits = 0;
        if (Events::g_event_group) {
            bits = xEventGroupWaitBits(Events::g_event_group, Events::BIT_WIFI_IP,
                                       pdTRUE, pdFALSE,
                                       pdMS_TO_TICKS(Timing::WEATHER_UPDATE_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(Timing::WEATHER_UPDATE_MS));
        }
        if (!m_wifi.has_ip()) continue;

        if (bits & Events::BIT_WIFI_IP) {
            ip_ts_us = esp_timer_get_time();  // woke on fresh IP signal
        }
        if (!m_geolocated) {
            geolocate();
            m_geolocated = true;
        }
        bool ok = update_now();
        if (ip_ts_us != 0) {
            if (ok) {
                ESP_LOGI(TAG, "Weather fetched %.0f ms after WiFi IP",
                         static_cast<double>(esp_timer_get_time() - ip_ts_us) / 1000.0);
            }
            ip_ts_us = 0;
        }
    }
}

} // namespace Fuchey
