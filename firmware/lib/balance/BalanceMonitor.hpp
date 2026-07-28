#pragma once

#include <cstdint>
#include <string>

namespace Fuchey {

class WiFiManager;

class BalanceMonitor {
public:
    BalanceMonitor(WiFiManager& wifi, const std::string& wallet_addr,
                   const std::string& usdc_mint, const std::string& rpc_url);
    ~BalanceMonitor() = default;

    BalanceMonitor(const BalanceMonitor&) = delete;
    BalanceMonitor& operator=(const BalanceMonitor&) = delete;

    void set_address(const std::string& addr) { m_wallet_addr = addr; }

    bool fetch_balances(double& sol_out, double& usdc_out);

private:
    WiFiManager&  m_wifi;
    std::string   m_wallet_addr;
    std::string   m_usdc_mint;
    std::string   m_rpc_url;

    double fetch_sol_balance();
    double fetch_usdc_balance();

    static constexpr const char* TAG = "BalanceMonitor";
};

} // namespace Fuchey
