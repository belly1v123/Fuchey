#include "IncomingWatcher.hpp"
#include "BalanceMonitor.hpp"
#include "../wifi/WiFiManager.hpp"
#include "../events/Events.hpp"
#include "../crypto/Base58.hpp"
#include "../wallet_manager/TxParser.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include <cstdio>
#include <cstring>

namespace Fuchey {

namespace {
// https://host/path → wss://host/path (Solana RPCs serve both on one URL).
std::string ws_url(const std::string& rpc) {
    if (rpc.rfind("https://", 0) == 0) return "wss://" + rpc.substr(8);
    if (rpc.rfind("http://", 0) == 0) return "ws://" + rpc.substr(7);
    return rpc;
}

// The wallet's USDC associated token account, or "" if an address is bad.
std::string usdc_ata(const std::string& owner, const std::string& mint) {
    const auto o = Crypto::Base58::decode(owner);
    const auto m = Crypto::Base58::decode(mint);
    if (o.size() != 32 || m.size() != 32) return {};
    Crypto::PubKey op{}, mp{}, ata{};
    std::memcpy(op.data(), o.data(), 32);
    std::memcpy(mp.data(), m.data(), 32);
    if (!TxParser::derive_ata(op, mp, ata)) return {};
    return Crypto::Base58::encode(ata);
}
} // namespace

void IncomingWatcher::start() {
    TaskHandle_t h = nullptr;
    xTaskCreatePinnedToCore(task_entry, "incoming", 8192, this,
                            tskIDLE_PRIORITY + 1, &h, 1);
    m_task = h;
}

void IncomingWatcher::task_entry(void* arg) {
    static_cast<IncomingWatcher*>(arg)->run();
}

void IncomingWatcher::run() {
    TickType_t last_check = xTaskGetTickCount();
    while (true) {
        // Woken early by a push (ws_event), otherwise every kTickMs.
        const bool pushed = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kTickMs)) > 0;

        if (!m_wifi.has_ip()) {
            close_socket();
            continue;
        }
        const BalanceMonitor::Target t = m_monitor.target();
        if (t.addr.empty()) {
            close_socket();
            continue;
        }

        // New wallet or network: fresh baseline and a socket on the new RPC,
        // so a different balance isn't mistaken for money arriving.
        const std::string key = t.addr + "|" + t.rpc + "|" + t.mint;
        if (key != m_key) {
            m_key = key;
            m_have = false;
            close_socket();
        }
        if (m_ws == nullptr) open_socket(t.rpc, t.addr, t.mint);

        const bool backup_due = (xTaskGetTickCount() - last_check) >= pdMS_TO_TICKS(kBackupMs);
        if (pushed || !m_have || backup_due) {
            last_check = xTaskGetTickCount();
            check();
        }
    }
}

void IncomingWatcher::check() {
    const BalanceMonitor::Target t = m_monitor.target();
    uint64_t lamports = 0, usdc = 0;
    if (!m_monitor.fetch_raw(t, lamports, usdc)) return;

    if (m_have && (lamports > m_lamports || usdc > m_usdc_units)) {
        Events::Event evt{};
        evt.type = Events::EventType::FUNDS_RECEIVED;
        evt.data.funds.sol  = lamports > m_lamports ? (lamports - m_lamports) / 1e9 : 0.0;
        evt.data.funds.usdc = usdc > m_usdc_units ? (usdc - m_usdc_units) / 1e6 : 0.0;
        ESP_LOGI(TAG, "Received +%.9f SOL, +%.6f USDC",
                 evt.data.funds.sol, evt.data.funds.usdc);
        Events::post(Events::g_ui_queue, evt);
    } else if (!m_have) {
        ESP_LOGI(TAG, "Baseline: %llu lamports, %llu USDC units (%s)",
                 static_cast<unsigned long long>(lamports),
                 static_cast<unsigned long long>(usdc),
                 m_live ? "live" : "socket not connected yet");
    }
    m_have = true;
    m_lamports = lamports;
    m_usdc_units = usdc;
}

// ── WebSocket ─────────────────────────────────────────────
void IncomingWatcher::open_socket(const std::string& rpc, const std::string& addr,
                                  const std::string& mint) {
    m_sub_wallet = addr;
    m_sub_ata = usdc_ata(addr, mint);
    const std::string url = ws_url(rpc);

    // The debug build logs every WebSocket header and ping; keep it to info.
    esp_log_level_set("websocket_client", ESP_LOG_INFO);
    esp_log_level_set("transport_ws", ESP_LOG_INFO);

    esp_websocket_client_config_t cfg{};
    cfg.uri = url.c_str();
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.task_stack = 6144;
    cfg.buffer_size = 2048;
    cfg.reconnect_timeout_ms = 5000;
    cfg.network_timeout_ms = 10000;
    cfg.ping_interval_sec = 20;
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
    if (ws == nullptr) return;
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, this);
    if (esp_websocket_client_start(ws) != ESP_OK) {
        esp_websocket_client_destroy(ws);
        return;
    }
    m_ws = ws;
    ESP_LOGI(TAG, "Opening %s", url.c_str());
}

void IncomingWatcher::close_socket() {
    if (m_ws == nullptr) return;
    auto ws = static_cast<esp_websocket_client_handle_t>(m_ws);
    m_ws = nullptr;
    m_live = false;
    esp_websocket_client_stop(ws);
    esp_websocket_client_destroy(ws);
}

// Called on every (re)connect: the server forgets subscriptions with the socket.
void IncomingWatcher::subscribe() {
    auto ws = static_cast<esp_websocket_client_handle_t>(m_ws);
    if (ws == nullptr) return;
    char req[256];
    int id = 1;
    for (const std::string* a : {&m_sub_wallet, &m_sub_ata}) {
        if (a->empty()) continue;
        const int n = snprintf(req, sizeof(req),
            "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"accountSubscribe\","
            "\"params\":[\"%s\",{\"encoding\":\"base64\",\"commitment\":\"confirmed\"}]}",
            id++, a->c_str());
        esp_websocket_client_send_text(ws, req, n, pdMS_TO_TICKS(5000));
    }
}

void IncomingWatcher::ws_event(void* arg, const char*, int32_t id, void* data) {
    auto* self = static_cast<IncomingWatcher*>(arg);
    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            self->m_live = true;
            ESP_LOGI(TAG, "Live: watching wallet%s", self->m_sub_ata.empty() ? "" : " + USDC account");
            self->subscribe();
            // Catch anything that arrived while the socket was down.
            if (self->m_task) xTaskNotifyGive(static_cast<TaskHandle_t>(self->m_task));
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_ERROR:
            if (self->m_live) ESP_LOGW(TAG, "Live link dropped; reconnecting");
            self->m_live = false;
            break;
        case WEBSOCKET_EVENT_DATA: {
            auto* d = static_cast<esp_websocket_event_data_t*>(data);
            // Any account push means "something changed": the task re-reads
            // the balances and decides (it ignores spends and fees).
            if (d->op_code == 0x1 && d->data_ptr && d->data_len > 0 &&
                memmem(d->data_ptr, d->data_len, "accountNotification", 19) != nullptr &&
                self->m_task) {
                xTaskNotifyGive(static_cast<TaskHandle_t>(self->m_task));
            }
            break;
        }
        default:
            break;
    }
}

} // namespace Fuchey
