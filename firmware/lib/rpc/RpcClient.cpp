// ============================================================
// Fuchey — RpcClient.cpp
// ============================================================

#include "RpcClient.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstring>
#include <cctype>

namespace Fuchey {

static constexpr const char* TAG = "RpcClient";

RpcClient::RpcClient(WiFiManager& wifi,
                     std::function<const std::vector<const char*>&()> url_resolver)
    : m_wifi(wifi), m_url_resolver(std::move(url_resolver)) {}

bool RpcClient::body_has_result(const std::string& body) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return false;
    bool has = cJSON_GetObjectItem(root, "result") != nullptr;
    cJSON_Delete(root);
    return has;
}

bool RpcClient::contains_ci(const char* haystack, const char* needle) {
    if (!haystack || !needle) return false;
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen) return false;
    for (size_t i = 0; i + nlen <= hlen; ++i) {
        bool match = true;
        for (size_t j = 0; j < nlen; ++j) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool RpcClient::body_has_retryable_error(const std::string& body) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return false;
    bool retryable = false;
    cJSON* err = cJSON_GetObjectItem(root, "error");
    if (err) {
        cJSON* code = cJSON_GetObjectItem(err, "code");
        cJSON* msg  = cJSON_GetObjectItem(err, "message");
        if (code && code->valueint == -32005) {
            retryable = true; // Solana: "node reports itself unhealthy"
        } else if (msg && msg->valuestring) {
            retryable = contains_ci(msg->valuestring, "rate limit") ||
                        contains_ci(msg->valuestring, "too many requests");
        }
    }
    cJSON_Delete(root);
    return retryable;
}

HttpResponse RpcClient::call(const char* jsonrpc_body, bool is_send_tx) {
    const auto& urls = m_url_resolver();
    HttpResponse last{.status_code = -1, .body = "", .success = false};
    const int64_t start_us = esp_timer_get_time();

    for (const char* url : urls) {
        if (esp_timer_get_time() - start_us > WALL_CLOCK_BUDGET_US) {
            ESP_LOGW(TAG, "RPC wall-clock budget exceeded — giving up");
            break;
        }

        auto resp = m_wifi.post_json(url, jsonrpc_body, nullptr,
                                     ATTEMPT_TIMEOUT_MS, MAX_ATTEMPTS_PER_URL);
        last = resp;

        if (resp.success) {
            if (body_has_result(resp.body)) return resp; // genuine success

            if (!is_send_tx) {
                ESP_LOGW(TAG, "RPC error on %s (read) — trying next endpoint", url);
                continue;
            }
            if (body_has_retryable_error(resp.body)) {
                ESP_LOGW(TAG, "RPC rate-limit/unhealthy on %s — trying next endpoint", url);
                continue;
            }
            // Terminal transaction error (e.g. insufficient funds) — surface it.
            return resp;
        }

        ESP_LOGW(TAG, "HTTP failure on %s — trying next endpoint", url);
    }

    return last;
}

} // namespace Fuchey