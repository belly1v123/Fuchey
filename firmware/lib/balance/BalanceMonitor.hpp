#pragma once

#include <cstdint>
#include <string>
#include <functional>

#include "../wifi/WiFiManager.hpp"

namespace Fuchey {

class BalanceMonitor {
public:
    // rpc_call routes all Solana RPC traffic (network-aware failover
    // lives in main.cpp's wrapper). Body + is_send_tx -> response.
    using RpcCall = std::function<HttpResponse(const char*, bool)>;

    BalanceMonitor(WiFiManager& wifi, RpcCall rpc_call,
                   const std::string& wallet_addr,
                   const std::string& usdc_mint);
    ~BalanceMonitor() = default;

    BalanceMonitor(const BalanceMonitor&) = delete;
    BalanceMonitor& operator=(const BalanceMonitor&) = delete;

    void set_address(const std::string& addr) { m_wallet_addr = addr; }

    bool fetch_balances(double& sol_out, double& usdc_out);

private:
    WiFiManager&  m_wifi;
    RpcCall       m_rpc_call;
    std::string   m_wallet_addr;
    std::string   m_usdc_mint;

    double fetch_sol_balance();
    double fetch_usdc_balance();

    static constexpr const char* TAG = "BalanceMonitor";
};

} // namespace Fuchey
