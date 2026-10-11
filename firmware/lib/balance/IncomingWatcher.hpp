#pragma once
// ============================================================
// Fuchey — IncomingWatcher.hpp
// Live detection of incoming money. Keeps a WebSocket open to the RPC
// (accountSubscribe on the wallet and its USDC token account); the server
// pushes a notification the moment either account changes. Each push (and
// a slow backup check, in case the link is down) re-reads the raw SOL +
// USDC balance and posts FUNDS_RECEIVED to the UI queue when either went
// up since the last good read.
// The first read (and the first after a wallet / network change) only
// sets the baseline; failed reads are skipped, never treated as 0.
// ============================================================

#include <atomic>
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

    void start();   // spawns the watcher task (core 1, low priority)

private:
    static constexpr uint32_t kBackupMs = 60000;  // re-read even without a push
    static constexpr uint32_t kTickMs   = 2000;   // wallet / network change check
    static constexpr const char* TAG = "Incoming";

    static void task_entry(void* arg);
    static void ws_event(void* arg, const char* base, int32_t id, void* data);
    void run();
    void check();
    void open_socket(const std::string& rpc, const std::string& addr, const std::string& mint);
    void close_socket();
    void subscribe();

    BalanceMonitor& m_monitor;
    WiFiManager&    m_wifi;
    void*           m_task{nullptr};    // TaskHandle_t
    void*           m_ws{nullptr};      // esp_websocket_client_handle_t
    std::string     m_sub_wallet;       // addresses subscribed on (re)connect
    std::string     m_sub_ata;
    std::atomic<bool> m_live{false};    // socket connected

    std::string     m_key;              // address|rpc|mint the baseline belongs to
    bool            m_have{false};
    uint64_t        m_lamports{0};
    uint64_t        m_usdc_units{0};
};

} // namespace Fuchey
