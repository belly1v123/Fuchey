#pragma once
// ============================================================
// Fuchey — PriceService.hpp
// Periodic HTTP fetch of SOL/USD price via CoinGecko API.
// ============================================================

#include "../wifi/WiFiManager.hpp"
#include "../events/Events.hpp"
#include <cstdint>
#include <atomic>

namespace Fuchey {

class PriceService {
public:
    explicit PriceService(WiFiManager& wifi);
    ~PriceService() = default;

    bool init();
    bool update_now();

    // Live SOL/USD price as of the most recent successful fetch.
    // Falls back to DEFAULT_SOL_USD if never fetched.
    float get_sol_usd() const { return m_sol_usd.load(); }

    static void task_entry(void* arg);
    void run();

    static constexpr float DEFAULT_SOL_USD = 150.0f;

private:
    WiFiManager& m_wifi;
    std::atomic<float> m_sol_usd;

    bool parse_price_json(const std::string& json_str, float& out_price);

    static constexpr const char* TAG = "PriceService";
};

} // namespace Fuchey
