#include "BalanceMonitor.hpp"
#include "../wifi/WiFiManager.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

namespace Fuchey {

BalanceMonitor::BalanceMonitor(WiFiManager& wifi, const std::string& wallet_addr,
                               const std::string& usdc_mint, const std::string& rpc_url)
    : m_wifi(wifi), m_wallet_addr(wallet_addr), m_usdc_mint(usdc_mint),
      m_rpc_url(rpc_url) {}

bool BalanceMonitor::fetch_balances(double& sol_out, double& usdc_out) {
    if (m_wallet_addr.empty()) return false;
    if (!m_wifi.has_ip()) return false;

    sol_out = fetch_sol_balance();
    usdc_out = fetch_usdc_balance();

    ESP_LOGI(TAG, "Balances — SOL: %.6f, USDC: $%.2f", sol_out, usdc_out);
    return true;
}

double BalanceMonitor::fetch_sol_balance() {
    char req[256];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getBalance\",\"params\":[\"%s\"]}",
             m_wallet_addr.c_str());

    auto resp = m_wifi.post_json(m_rpc_url.c_str(), req);
    if (!resp.success) return 0.0;

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) return 0.0;

    double bal = 0.0;
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
    if (!resp.success) return 0.0;

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) return 0.0;

    double bal = 0.0;
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
