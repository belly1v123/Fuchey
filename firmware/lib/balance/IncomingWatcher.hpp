#pragma once
// ============================================================
// Fuchey — IncomingWatcher.hpp
// Background check for incoming money: every 30 s (WiFi up, wallet
// present) reads the raw SOL + USDC balance and posts FUNDS_RECEIVED to
// the UI queue when either went up since the last good read.
// The first read (and the first after a wallet / network change) only
// sets the baseline; failed reads are skipped, never treated as 0.
// ============================================================

#include <cstdint>
#include <string>

namespace Fuchey {

class BalanceMonitor;
class WiFiManager;

class IncomingWatcher {
public:
    IncomingWatcher(BalanceMonitor& monitor, WiFiManager& wifi)
        : m_monitor(monitor), m_wifi(wifi) {}

    IncomingWatcher(const IncomingWatcher&) = delete;
    IncomingWatcher& operator=(const IncomingWatcher&) = delete;

    void start();   // spawns the polling task (core 1, low priority)

private:
    static constexpr uint32_t kPollMs = 30000;
    static constexpr const char* TAG = "Incoming";

    static void task_entry(void* arg);
    void run();
    void check();

    BalanceMonitor& m_monitor;
    WiFiManager&    m_wifi;
    std::string     m_key;          // address|rpc|mint the baseline belongs to
    bool            m_have{false};
    uint64_t        m_lamports{0};
    uint64_t        m_usdc_units{0};
};

} // namespace Fuchey
