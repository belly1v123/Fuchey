#pragma once
// ============================================================
// Fuchey — WeatherService.hpp
// Periodic HTTP fetch of current weather data using Open-Meteo API.
// ============================================================

#include "../wifi/WiFiManager.hpp"
#include "../events/Events.hpp"
#include <cstdint>

namespace Fuchey {

class WeatherService {
public:
    explicit WeatherService(WiFiManager& wifi);
    ~WeatherService() = default;

    bool init();
    bool update_now();

    static void task_entry(void* arg);
    void run();

private:
    WiFiManager& m_wifi;
    float m_lat{40.7128f};  // Default: New York
    float m_lon{-74.0060f};

    bool parse_weather_json(const std::string& json_str, Events::Event& evt);

    static constexpr const char* TAG = "WeatherService";
};

} // namespace Fuchey
