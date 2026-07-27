#pragma once
// ============================================================
// Fuchey — BalanceMonitor.hpp
// Periodically polls SOL + USDC balances via RPC and posts
// FUNDS_RECEIVED events when balance increases.
// Follows the same pattern as PriceService / WeatherService.
// ============================================================

#include <cstdint>
#include <string>

namespace Fuchey {

class WiFiManager;

class BalanceMonitor {
public:
    BalanceMonitor(WiFiManager& wifi, const std::string& wallet_addr,
                   const std::string& usdc_mint, const std::string& rpc_url,
                   double sol_threshold = 0.001, double usdc_threshold = 0.01);
    ~BalanceMonitor() = default;

    BalanceMonitor(const BalanceMonitor&) = delete;
    BalanceMonitor& operator=(const BalanceMonitor&) = delete;

    void init();
    void set_address(const std::string& addr) { m_wallet_addr = addr; }
    void run();
    static void task_entry(void* arg);

private:
    WiFiManager&  m_wifi;
    std::string   m_wallet_addr;
    std::string   m_usdc_mint;
    std::string   m_rpc_url;
    double        m_sol_thresh;
    double        m_usdc_thresh;

    double m_prev_sol{0.0};
    double m_prev_usdc{0.0};
    bool   m_first_done{false};

    void poll();
    double fetch_sol_balance();
    double fetch_usdc_balance();

    static constexpr const char* TAG = "BalanceMonitor";
};

} // namespace Fuchey
