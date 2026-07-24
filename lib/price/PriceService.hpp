#pragma once
// ============================================================
// Fuchey — PriceService.hpp
// Periodic HTTP fetch of SOL/USD price via CoinGecko API.
// ============================================================

#include "../wifi/WiFiManager.hpp"
#include "../events/Events.hpp"
#include <cstdint>

namespace Fuchey {

class PriceService {
public:
    explicit PriceService(WiFiManager& wifi);
    ~PriceService() = default;

    bool init();
    bool update_now();

    static void task_entry(void* arg);
    void run();

private:
    WiFiManager& m_wifi;

    bool parse_price_json(const std::string& json_str, float& out_price);

    static constexpr const char* TAG = "PriceService";
};

} // namespace Fuchey
