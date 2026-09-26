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
    ESP_LOGI(TAG, "WeatherService initialized (city=%s, source=%s, lat=%.4f, lon=%.4f)",
             m_city_name.c_str(), m_location_source.c_str(), m_lat, m_lon);
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
    if (!cfg.is_open() || !cfg.get_str(NVS::KEY_WEATHER_LAT) || !cfg.get_str(NVS::KEY_WEATHER_LON)) {
        ESP_LOGI(TAG, "No saved location in NVS — using default: %s", m_city_name.c_str());
        return;
    }

    auto src = cfg.get_str(NVS::KEY_WEATHER_SOURCE);
    // Coords left by the old IP-geolocation firmware are stale (they may point
    // at the ISP gateway, not the user). Treat them as unconfigured so the
    // first-boot setup wizard asks for a real location again.
    if (src && *src == "geolocation") {
        ESP_LOGW(TAG, "Ignoring stale geolocation coords — location needs setup");
        return;
    }

    auto city = cfg.get_str(NVS::KEY_WEATHER_CITY);
    if (city) m_city_name = std::move(*city);

    auto lat_str = cfg.get_str(NVS::KEY_WEATHER_LAT);
    if (lat_str) m_lat = std::stof(*lat_str);

    auto lon_str = cfg.get_str(NVS::KEY_WEATHER_LON);
    if (lon_str) m_lon = std::stof(*lon_str);

    m_location_source    = (src && *src == "manual") ? std::move(*src) : "saved (NVS)";
    m_location_configured = true;
}

bool WeatherService::set_manual_location(const char* city, float lat, float lon) {
    if (!city || !*city || lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) {
        ESP_LOGE(TAG, "Invalid manual location: city='%s' lat=%.4f lon=%.4f",
                 city ? city : "(null)", lat, lon);
        return false;
    }
    set_location(city, lat, lon);
    m_location_source = "manual";
    m_location_configured = true;
    save_config();
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

    snprintf(evt.data.weather.city, sizeof(evt.data.weather.city), "%s", m_city_name.c_str());

    cJSON_Delete(root);
    return true;
}

bool WeatherService::update_now() {
    if (!m_wifi.has_ip()) return false;

    char url[256];
    snprintf(url, sizeof(url), API::WEATHER_URL_FMT, m_lat, m_lon);
    ESP_LOGI(TAG, "Weather source: %s — fetching %s (%.4f, %.4f)",
             m_location_source.c_str(), m_city_name.c_str(), m_lat, m_lon);

    // Retry on transient network/DNS failures. The cycle is 10 min, so a
    // short retry window is cheap and self-heals DNS blips.
    static constexpr int MAX_ATTEMPTS = 3;
    for (int attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt) {
        auto resp = m_wifi.get(url);
        if (!resp.success) {
            if (attempt < MAX_ATTEMPTS) {
                ESP_LOGW(TAG, "Weather fetch failed (attempt %d/%d) — retrying in 2500ms...",
                         attempt, MAX_ATTEMPTS);
                vTaskDelay(pdMS_TO_TICKS(2500));
            } else {
                ESP_LOGE(TAG, "Weather HTTP request failed after %d attempts", MAX_ATTEMPTS);
            }
            continue;
        }

        Events::Event evt{};
        evt.type = Events::EventType::WEATHER_UPDATED;

        if (parse_weather_json(resp.body, evt)) {
            ESP_LOGI(TAG, "Weather updated: %.1f °C, Code: %d [%s]",
                     evt.data.weather.temp_celsius, evt.data.weather.weather_code,
                     m_location_source.c_str());
            Events::post(Events::g_ui_queue, evt);
            if (Events::g_event_group) {
                xEventGroupSetBits(Events::g_event_group, Events::BIT_WEATHER_OK);
            }
            return true;
        }

        ESP_LOGE(TAG, "Weather JSON parse failed");
        return false;
    }

    return false;
}

void WeatherService::task_entry(void* arg) {
    static_cast<WeatherService*>(arg)->run();
}

void WeatherService::save_config() {
    Storage::Handle cfg(NVS::CONFIG_NS, NVS_READWRITE);
    if (!cfg.is_open()) {
        ESP_LOGW(TAG, "Cannot persist location: NVS not open");
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4f", m_lat);
    cfg.set_str(NVS::KEY_WEATHER_CITY, m_city_name.c_str());
    cfg.set_str(NVS::KEY_WEATHER_LAT, buf);
    snprintf(buf, sizeof(buf), "%.4f", m_lon);
    cfg.set_str(NVS::KEY_WEATHER_LON, buf);
    cfg.set_str(NVS::KEY_WEATHER_SOURCE, m_location_source.c_str());
    cfg.commit();
}

void WeatherService::run() {
    ESP_LOGI(TAG, "WeatherService task running");
    while (true) {
        if (Events::g_event_group) {
            // Woke on fresh IP signal or timer tick — just loop and fetch.
            xEventGroupWaitBits(Events::g_event_group, Events::BIT_WIFI_IP,
                                pdTRUE, pdFALSE,
                                pdMS_TO_TICKS(Timing::WEATHER_UPDATE_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(Timing::WEATHER_UPDATE_MS));
        }
        if (!m_wifi.has_ip()) continue;

        update_now();
    }
}

} // namespace Fuchey
