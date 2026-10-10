#include "BalanceMonitor.hpp"
#include "../wifi/WiFiManager.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Fuchey {

BalanceMonitor::BalanceMonitor(WiFiManager& wifi, const std::string& wallet_addr,
                               const std::string& usdc_mint, const std::string& rpc_url)
    : m_wifi(wifi), m_wallet_addr(wallet_addr), m_usdc_mint(usdc_mint),
      m_rpc_url(rpc_url) {}

bool BalanceMonitor::fetch_balances(double& sol_out, double& usdc_out) {
    const Target t = target();
    if (t.addr.empty()) return false;
    if (!m_wifi.has_ip()) return false;

    sol_out = fetch_sol_balance(t);
    usdc_out = fetch_usdc_balance(t);

    ESP_LOGI(TAG, "Balances — SOL: %.6f, USDC: $%.2f", sol_out, usdc_out);
    return true;
}

double BalanceMonitor::fetch_sol_balance(const Target& t) {
    char req[256];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getBalance\",\"params\":[\"%s\"]}",
             t.addr.c_str());

    auto resp = m_wifi.post_json(t.rpc.c_str(), req);
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

double BalanceMonitor::fetch_usdc_balance(const Target& t) {
    char req[512];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTokenAccountsByOwner\","
             "\"params\":[\"%s\",{\"mint\":\"%s\"},{\"encoding\":\"jsonParsed\"}]}",
             t.addr.c_str(), t.mint.c_str());

    auto resp = m_wifi.post_json(t.rpc.c_str(), req);
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

bool BalanceMonitor::fetch_raw(const Target& t, uint64_t& lamports, uint64_t& usdc_units) {
    if (t.addr.empty() || !m_wifi.has_ip()) return false;
    char req[512];

    // SOL: result.value is the lamport count (exact in a double below 2^53).
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getBalance\",\"params\":[\"%s\"]}",
             t.addr.c_str());
    auto resp = m_wifi.post_json(t.rpc.c_str(), req);
    if (!resp.success) return false;
    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) return false;
    cJSON* result = cJSON_GetObjectItem(root, "result");
    cJSON* value  = result ? cJSON_GetObjectItem(result, "value") : nullptr;
    const bool sol_ok = value && cJSON_IsNumber(value) && value->valuedouble >= 0;
    if (sol_ok) lamports = static_cast<uint64_t>(value->valuedouble);
    cJSON_Delete(root);
    if (!sol_ok) return false;

    // USDC: sum tokenAmount.amount (a decimal string) over every account of
    // the mint. No account at all is a valid zero.
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTokenAccountsByOwner\","
             "\"params\":[\"%s\",{\"mint\":\"%s\"},{\"encoding\":\"jsonParsed\"}]}",
             t.addr.c_str(), t.mint.c_str());
    resp = m_wifi.post_json(t.rpc.c_str(), req);
    if (!resp.success) return false;
    root = cJSON_Parse(resp.body.c_str());
    if (!root) return false;
    result = cJSON_GetObjectItem(root, "result");
    cJSON* list = result ? cJSON_GetObjectItem(result, "value") : nullptr;
    bool ok = cJSON_IsArray(list);
    uint64_t total = 0;
    cJSON* item = nullptr;
    if (ok) cJSON_ArrayForEach(item, list) {
        cJSON* account = cJSON_GetObjectItem(item, "account");
        cJSON* data    = account ? cJSON_GetObjectItem(account, "data")   : nullptr;
        cJSON* parsed  = data    ? cJSON_GetObjectItem(data,    "parsed") : nullptr;
        cJSON* info    = parsed  ? cJSON_GetObjectItem(parsed,  "info")   : nullptr;
        cJSON* t_amt   = info    ? cJSON_GetObjectItem(info,    "tokenAmount") : nullptr;
        cJSON* amount  = t_amt   ? cJSON_GetObjectItem(t_amt,   "amount") : nullptr;
        if (!cJSON_IsString(amount)) { ok = false; break; }
        total += strtoull(amount->valuestring, nullptr, 10);
    }
    cJSON_Delete(root);
    if (!ok) return false;
    usdc_units = total;
    return true;
}

} // namespace Fuchey
