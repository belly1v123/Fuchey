#pragma once

#include <cstdint>
#include <mutex>
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

    // Wallet + network the monitor reads. Copied under a lock: the setters
    // run on other tasks than the fetch worker and IncomingWatcher.
    struct Target {
        std::string addr;
        std::string rpc;
        std::string mint;
    };
    Target target() const {
        std::lock_guard<std::mutex> lock(m_mu);
        return {m_wallet_addr, m_rpc_url, m_usdc_mint};
    }

    void set_address(const std::string& addr) {
        std::lock_guard<std::mutex> lock(m_mu);
        m_wallet_addr = addr;
    }
    // Re-point at a different network (RPC URL + USDC mint). Called after
    // the NVS network load at boot and on every console `network` switch —
    // the constructor snapshot alone would pin the monitor to devnet.
    void set_network(const std::string& rpc_url, const std::string& usdc_mint) {
        std::lock_guard<std::mutex> lock(m_mu);
        m_rpc_url = rpc_url;
        m_usdc_mint = usdc_mint;
    }

    bool fetch_balances(double& sol_out, double& usdc_out);

    // Exact on-chain amounts (lamports, USDC base units) for change
    // detection. False on any HTTP or parse failure, so a failed read is
    // never mistaken for an empty wallet.
    bool fetch_raw(const Target& t, uint64_t& lamports, uint64_t& usdc_units);

private:
    WiFiManager&  m_wifi;
    mutable std::mutex m_mu;
    std::string   m_wallet_addr;
    std::string   m_usdc_mint;
    std::string   m_rpc_url;

    double fetch_sol_balance(const Target& t);
    double fetch_usdc_balance(const Target& t);

    static constexpr const char* TAG = "BalanceMonitor";
};

} // namespace Fuchey
