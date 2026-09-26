#pragma once
// ============================================================
// Fuchey — WeatherService.hpp
// Periodic HTTP fetch of current weather data using Open-Meteo API.
// ============================================================

#include "../wifi/WiFiManager.hpp"
#include "../events/Events.hpp"
#include "../config/Config.hpp"
#include "../storage/Storage.hpp"
#include <cstdint>
#include <string>

namespace Fuchey {

class WeatherService {
public:
    explicit WeatherService(WiFiManager& wifi);
    ~WeatherService() = default;

    bool init();
    bool update_now();

    void load_config();
    void save_config();

    static void task_entry(void* arg);
    void run();

    const std::string& city_name() const { return m_city_name; }
    const std::string& location_source() const { return m_location_source; }
    void set_location(const char* city, float lat, float lon);
    bool has_configured_location() const { return m_location_configured; }
    bool set_manual_location(const char* city, float lat, float lon);

private:
    WiFiManager& m_wifi;
    float m_lat{27.7172f};   // Default: Kathmandu, Nepal
    float m_lon{85.3240f};
    std::string m_city_name{"Kathmandu"};
    // Where the current coords came from: "default" or "manual" (NVS-persisted
    // user location). IP geolocation was removed; nothing else sets this.
    std::string m_location_source{"default"};
    bool m_location_configured{false};

    bool parse_weather_json(const std::string& json_str, Events::Event& evt);

    static constexpr const char* TAG = "WeatherService";
};

} // namespace Fuchey
