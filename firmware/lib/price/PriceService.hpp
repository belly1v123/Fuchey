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
    // Non-blocking: wakes the price task for an immediate background fetch.
    // Safe to call from the UI task (no HTTP here). Throttle at the caller.
    void request_update();

    // Live SOL/USD price as of the most recent successful fetch.
    // Returns DEFAULT_SOL_USD when nothing was fetched yet (TX conversion
    // needs a sane number); use has_data() to distinguish real vs fallback.
    float get_sol_usd() const {
        float v = m_sol_usd.load();
        return v < 0.0f ? DEFAULT_SOL_USD : v;
    }
    // True once at least one fetch succeeded (price/high/low/change are live).
    bool has_data() const { return m_sol_usd.load() >= 0.0f; }
    // 24h market context from Binance 24hr ticker. Negative = never fetched.
    float get_high_24h() const { return m_high_24h.load(); }
    float get_low_24h() const { return m_low_24h.load(); }
    float get_change_pct_24h() const { return m_change_pct.load(); }

    static void task_entry(void* arg);
    void run();

    static constexpr float DEFAULT_SOL_USD = 150.0f;

private:
    WiFiManager& m_wifi;
    std::atomic<float> m_sol_usd{-1.0f};
    std::atomic<float> m_high_24h{-1.0f};
    std::atomic<float> m_low_24h{-1.0f};
    std::atomic<float> m_change_pct{0.0f};

    bool parse_price_json(const std::string& json_str, float& out_price,
                          float& out_high, float& out_low, float& out_change_pct);

    static constexpr const char* TAG = "PriceService";
};

} // namespace Fuchey
