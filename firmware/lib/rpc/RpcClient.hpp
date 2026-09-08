#pragma once
// ============================================================
// Fuchey — RpcClient.hpp
// Resilient Solana JSON-RPC client with endpoint failover.
// Iterates the endpoint list for the active network, retrying
// each URL with a reduced attempt count, bounded by a wall-clock
// budget so the caller (and the UI) never hangs.
// ============================================================

#include "../wifi/WiFiManager.hpp"
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace Fuchey {

class RpcClient {
public:
    // url_resolver returns the ordered endpoint list for the active
    // network; queried per call so network switches take effect live.
    RpcClient(WiFiManager& wifi,
              std::function<const std::vector<const char*>&()> url_resolver);

    // Per-URL budget, reduced attempt count and total wall-clock cap.
    static constexpr uint32_t ATTEMPT_TIMEOUT_MS    = 15000;
    static constexpr uint32_t MAX_ATTEMPTS_PER_URL  = 1;
    static constexpr int64_t  WALL_CLOCK_BUDGET_US  = 20 * 1000 * 1000; // 20s

    // is_send_tx controls failover on HTTP 200 + JSON-RPC error bodies:
    //  - reads: fail over on any error body (idempotent, cheap)
    //  - sendTransaction: fail over only on rate-limit / node-unhealthy
    //    signals; any other error is terminal and surfaced to the caller.
    HttpResponse call(const char* jsonrpc_body, bool is_send_tx);

private:
    WiFiManager& m_wifi;
    std::function<const std::vector<const char*>&()> m_url_resolver;

    static bool body_has_result(const std::string& body);
    static bool body_has_retryable_error(const std::string& body);
    static bool contains_ci(const char* haystack, const char* needle);
};

} // namespace Fuchey