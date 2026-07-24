// ============================================================
// Fuchey — WiFiManager.cpp
// ============================================================

#include "WiFiManager.hpp"
#include "../storage/Storage.hpp"
#include "../config/Config.hpp"
#include "../events/Events.hpp"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_crt_bundle.h"
#include <cstring>

namespace Fuchey {

static constexpr const char* TAG = "WiFiManager";

// Module-level pointer for event handler callback
static WiFiManager* s_instance = nullptr;

WiFiManager::~WiFiManager() {
    disconnect();
    if (m_initialized) {
        esp_wifi_stop();
        esp_wifi_deinit();
        m_initialized = false;
    }
}

// ─── Init ─────────────────────────────────────────────────
bool WiFiManager::init() {
    s_instance = this;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, this));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, this));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    m_initialized = true;
    ESP_LOGI(TAG, "WiFiManager initialized");
    return true;
}

// ─── Connect ──────────────────────────────────────────────
bool WiFiManager::connect(const char* ssid, const char* password) {
    wifi_config_t wifi_cfg{};
    std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.ssid),
                 ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.password),
                 password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable    = true;
    wifi_cfg.sta.pmf_cfg.required   = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    return true;
}

bool WiFiManager::connect_from_nvs() {
    Storage::Handle nvs(NVS::WIFI_NS, NVS_READONLY);
    if (!nvs.is_open()) return false;

    auto ssid = nvs.get_str(NVS::KEY_WIFI_SSID);
    auto pass = nvs.get_str(NVS::KEY_WIFI_PASS);

    if (!ssid || ssid->empty()) {
        ESP_LOGW(TAG, "No WiFi credentials in NVS");
        return false;
    }

    return connect(ssid->c_str(), pass ? pass->c_str() : "");
}

void WiFiManager::disconnect() {
    esp_wifi_disconnect();
    m_connected = false;
    m_has_ip    = false;
}

bool WiFiManager::save_credentials(const char* ssid, const char* password) {
    Storage::Handle nvs(NVS::WIFI_NS, NVS_READWRITE);
    if (!nvs.is_open()) return false;
    if (!nvs.set_str(NVS::KEY_WIFI_SSID, ssid)) return false;
    if (!nvs.set_str(NVS::KEY_WIFI_PASS, password)) return false;
    return nvs.commit();
}

// ─── Event handler ────────────────────────────────────────
void WiFiManager::wifi_event_handler(void* arg, esp_event_base_t base,
                                      int32_t event_id, void* event_data) {
    auto* self = static_cast<WiFiManager*>(arg);

    if (base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi started");
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi associated");
                self->m_connected = true;
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                self->m_connected = false;
                self->m_has_ip    = false;
                ESP_LOGW(TAG, "WiFi disconnected — reconnecting...");

                Events::Event evt{};
                evt.type = Events::EventType::WIFI_DISCONNECTED;
                if (Events::g_ui_queue) Events::post(Events::g_ui_queue, evt);
                if (Events::g_event_group)
                    xEventGroupClearBits(Events::g_event_group,
                                         Events::BIT_WIFI_CONNECTED | Events::BIT_WIFI_IP);

                // Auto-reconnect after delay
                vTaskDelay(pdMS_TO_TICKS(Timing::WIFI_RECONNECT_DELAY_MS));
                esp_wifi_connect();
                break;
            }
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        self->m_has_ip = true;

        Events::Event evt{};
        evt.type = Events::EventType::WIFI_GOT_IP;
        if (Events::g_ui_queue)    Events::post(Events::g_ui_queue, evt);
        if (Events::g_event_group) {
            xEventGroupSetBits(Events::g_event_group,
                               Events::BIT_WIFI_CONNECTED | Events::BIT_WIFI_IP);
        }
    }
}

// ─── HTTP GET ─────────────────────────────────────────────
HttpResponse WiFiManager::get(const char* url, const char* bearer_token,
                               uint32_t timeout_ms) {
    esp_http_client_config_t cfg{};
    cfg.url              = url;
    cfg.timeout_ms       = static_cast<int>(timeout_ms);
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    return do_request(cfg, "GET", nullptr, bearer_token, timeout_ms);
}

// ─── HTTP POST ────────────────────────────────────────────
HttpResponse WiFiManager::post_json(const char* url, const char* body,
                                     const char* bearer_token,
                                     uint32_t timeout_ms) {
    esp_http_client_config_t cfg{};
    cfg.url              = url;
    cfg.timeout_ms       = static_cast<int>(timeout_ms);
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    return do_request(cfg, "POST", body, bearer_token, timeout_ms);
}

// ─── Internal HTTP request ────────────────────────────────
HttpResponse WiFiManager::do_request(esp_http_client_config_t& cfg,
                                      const char* method,
                                      const char* body,
                                      const char* bearer_token,
                                      uint32_t timeout_ms) {
    HttpResponse response{.status_code = -1, .body = "", .success = false};

    if (!m_has_ip) {
        ESP_LOGW(TAG, "HTTP request attempted without IP — no network");
        return response;
    }

    auto client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return response;
    }

    esp_http_client_set_method(client,
        std::strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);

    if (bearer_token) {
        std::string auth = "Bearer ";
        auth += bearer_token;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
    }

    if (body && std::strcmp(method, "POST") == 0) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        response.status_code = esp_http_client_get_status_code(client);
        int64_t content_length = esp_http_client_get_content_length(client);
        if (content_length > 0 && content_length < static_cast<int64_t>(HTTP_BUF_SIZE)) {
            response.body.resize(static_cast<size_t>(content_length));
            esp_http_client_read_response(client,
                                           response.body.data(),
                                           static_cast<int>(content_length));
        }
        response.success = (response.status_code >= 200 && response.status_code < 300);
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return response;
}

} // namespace Fuchey
