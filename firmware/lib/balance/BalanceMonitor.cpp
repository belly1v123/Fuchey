// ============================================================
// Fuchey — BalanceMonitor.cpp
// Polls SOL + USDC balances every 30s. Posts FUNDS_RECEIVED
// event to g_ui_queue when a balance increase is detected.
// ============================================================

#include "BalanceMonitor.hpp"
#include "../wifi/WiFiManager.hpp"
#include "../events/Events.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstdio>
#include <cstring>

namespace Fuchey {

BalanceMonitor::BalanceMonitor(WiFiManager& wifi, const std::string& wallet_addr,
                               const std::string& usdc_mint, const std::string& rpc_url,
                               double sol_threshold, double usdc_threshold)
    : m_wifi(wifi), m_wallet_addr(wallet_addr), m_usdc_mint(usdc_mint),
      m_rpc_url(rpc_url), m_sol_thresh(sol_threshold), m_usdc_thresh(usdc_threshold) {}

void BalanceMonitor::init() {
    m_prev_sol = fetch_sol_balance();
    m_prev_usdc = fetch_usdc_balance();
    m_first_done = true;
    ESP_LOGI(TAG, "Initial balances — SOL: %.6f, USDC: $%.2f", m_prev_sol, m_prev_usdc);
}

void BalanceMonitor::task_entry(void* arg) {
    static_cast<BalanceMonitor*>(arg)->run();
}

void BalanceMonitor::run() {
    ESP_LOGI(TAG, "BalanceMonitor task running on Core %d", xPortGetCoreID());
    while (true) {
        if (m_wifi.has_ip()) {
            poll();
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void BalanceMonitor::poll() {
    if (m_wallet_addr.empty()) return;
    double sol = fetch_sol_balance();
    double usdc = fetch_usdc_balance();

    if (!m_first_done) {
        m_prev_sol = sol;
        m_prev_usdc = usdc;
        m_first_done = true;
        return;
    }

    double sol_change = sol - m_prev_sol;
    double usdc_change = usdc - m_prev_usdc;

    if (sol_change >= m_sol_thresh || usdc_change >= m_usdc_thresh) {
        ESP_LOGI(TAG, "Funds received! SOL: +%.6f, USDC: +$%.2f", sol_change, usdc_change);

        Events::Event evt{};
        evt.type = Events::EventType::FUNDS_RECEIVED;

        // We store raw doubles through the raw[] field
        // Layout: [sol_change 8B] [usdc_change 8B] [new_sol 8B] [new_usdc 8B]
        double vals[4] = { sol_change, usdc_change, sol, usdc };
        static_assert(sizeof(vals) <= sizeof(evt.data.raw), "vals too large");
        memcpy(evt.data.raw, vals, sizeof(vals));

        Events::post(Events::g_ui_queue, evt);

        m_prev_sol = sol;
        m_prev_usdc = usdc;
    } else {
        // Update even on no-change in case of small drifts
        m_prev_sol = sol;
        m_prev_usdc = usdc;
    }
}

double BalanceMonitor::fetch_sol_balance() {
    char req[256];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getBalance\",\"params\":[\"%s\"]}",
             m_wallet_addr.c_str());

    auto resp = m_wifi.post_json(m_rpc_url.c_str(), req);
    if (!resp.success) return m_prev_sol;

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) return m_prev_sol;

    double bal = m_prev_sol;
    cJSON* result = cJSON_GetObjectItem(root, "result");
    cJSON* value  = result ? cJSON_GetObjectItem(result, "value") : nullptr;
    if (value && cJSON_IsNumber(value)) {
        bal = value->valuedouble / 1000000000.0;
    }
    cJSON_Delete(root);
    return bal;
}

double BalanceMonitor::fetch_usdc_balance() {
    char req[512];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTokenAccountsByOwner\","
             "\"params\":[\"%s\",{\"mint\":\"%s\"},{\"encoding\":\"jsonParsed\"}]}",
             m_wallet_addr.c_str(), m_usdc_mint.c_str());

    auto resp = m_wifi.post_json(m_rpc_url.c_str(), req);
    if (!resp.success) return m_prev_usdc;

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) return m_prev_usdc;

    double bal = m_prev_usdc;
    cJSON* result = cJSON_GetObjectItem(root, "result");
    cJSON* val = result ? cJSON_GetObjectItem(result, "value") : nullptr;
    if (cJSON_IsArray(val) && cJSON_GetArraySize(val) > 0) {
        cJSON* item0   = cJSON_GetArrayItem(val, 0);
        cJSON* account = cJSON_GetObjectItem(item0, "account");
        cJSON* data    = account ? cJSON_GetObjectItem(account, "data")   : nullptr;
        cJSON* parsed  = data    ? cJSON_GetObjectItem(data,    "parsed") : nullptr;
        cJSON* info    = parsed  ? cJSON_GetObjectItem(parsed,  "info")   : nullptr;
        cJSON* t_amt   = info    ? cJSON_GetObjectItem(info,    "tokenAmount") : nullptr;
        cJSON* ui_amt  = t_amt   ? cJSON_GetObjectItem(t_amt,   "uiAmount")   : nullptr;
        if (ui_amt && cJSON_IsNumber(ui_amt)) {
            bal = ui_amt->valuedouble;
        }
    }
    cJSON_Delete(root);
    return bal;
}

} // namespace Fuchey
