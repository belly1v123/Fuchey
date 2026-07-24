#pragma once
// ============================================================
// Fuchey — WiFiManager.hpp
// Event-driven WiFi lifecycle. All HTTP requests go here.
// No other module calls esp_wifi_* or esp_http_client directly.
// ============================================================

#include <esp_wifi.h>
#include <esp_http_client.h>
#include <string>
#include <functional>
#include <cstdint>

namespace Fuchey {

// ─── HTTP Response ────────────────────────────────────────
struct HttpResponse {
    int         status_code;
    std::string body;
    bool        success;
};

class WiFiManager {
public:
    WiFiManager() = default;
    ~WiFiManager();

    // Non-copyable
    WiFiManager(const WiFiManager&) = delete;
    WiFiManager& operator=(const WiFiManager&) = delete;

    // ── Lifecycle ────────────────────────────────────────
    bool init();
    bool connect(const char* ssid, const char* password);
    bool connect_from_nvs(); // Loads SSID/password from NVS
    void disconnect();

    // ── Status ───────────────────────────────────────────
    bool is_connected() const { return m_connected; }
    bool has_ip()       const { return m_has_ip; }

    // ── Save credentials ──────────────────────────────────
    bool save_credentials(const char* ssid, const char* password);

    // ── HTTP ─────────────────────────────────────────────
    // Synchronous GET (blocks, suitable for FreeRTOS task)
    HttpResponse get(const char* url,
                     const char* bearer_token = nullptr,
                     uint32_t timeout_ms = 10000);

    // Synchronous POST with JSON body
    HttpResponse post_json(const char* url,
                           const char* body,
                           const char* bearer_token = nullptr,
                           uint32_t timeout_ms = 30000);

private:
    bool m_connected{false};
    bool m_has_ip{false};
    bool m_initialized{false};

    // ESP-IDF event handler (static)
    static void wifi_event_handler(void* arg,
                                   esp_event_base_t base,
                                   int32_t event_id,
                                   void* event_data);

    // HTTP helper
    HttpResponse do_request(esp_http_client_config_t& cfg,
                            const char* method,
                            const char* body,
                            const char* bearer_token,
                            uint32_t timeout_ms);

    static constexpr size_t HTTP_BUF_SIZE = 8192;
    static constexpr const char* TAG = "WiFiManager";
};

} // namespace Fuchey
