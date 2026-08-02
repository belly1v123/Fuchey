// ============================================================
// Fuchey — main.cpp
// System entry point. Boot sequence, queue allocation,
// subsystem initialization, and FreeRTOS task pinning.
// ============================================================

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "cJSON.h"
#include <cstring>
#include <cctype>
#include <string>

#include "../lib/config/Config.hpp"
#include "../lib/events/Events.hpp"
#include "../lib/storage/Storage.hpp"
#include "../lib/display/Display.hpp"
#include "../lib/display/ui/UIManager.hpp"
#include "../lib/buttons/ButtonDriver.hpp"
#include "../lib/crypto/CryptoEngine.hpp"
#include "../lib/crypto/Base58.hpp"
#include "../lib/crypto/SHA256.hpp"
#include "../lib/crypto/Ed25519.hpp"
#include "../lib/wallet/WalletCore.hpp"
#include "../lib/policy/SpendingPolicy.hpp"
#include "../lib/wallet_manager/WalletManager.hpp"
#include "../lib/wifi/WiFiManager.hpp"
#include "../lib/chat/AIManager.hpp"
#include "../lib/weather/WeatherService.hpp"
#include "../lib/price/PriceService.hpp"
#include "../lib/balance/BalanceMonitor.hpp"

namespace Fuchey {
namespace Events {

// Define global handles declared as extern in Events.hpp
QueueHandle_t g_wallet_queue = nullptr;
QueueHandle_t g_ui_queue     = nullptr;
QueueHandle_t g_ai_queue     = nullptr;
QueueHandle_t g_button_queue = nullptr;
EventGroupHandle_t g_event_group = nullptr;

} // namespace Events
} // namespace Fuchey

namespace Fuchey {
QueueHandle_t g_tx_confirm_queue = nullptr;
}
QueueHandle_t g_button_queue_ref = nullptr;

static constexpr const char* TAG = "FucheyMain";

// Core System Objects
static Fuchey::Display        s_display(Fuchey::DisplayConfig::I2C_PORT,
                                        Fuchey::DisplayConfig::I2C_ADDRESS,
                                        Fuchey::DisplayConfig::PIN_SDA,
                                        Fuchey::DisplayConfig::PIN_SCL,
                                        Fuchey::DisplayConfig::I2C_FREQ_HZ);
static Fuchey::UIManager      s_ui(s_display);
static Fuchey::ButtonDriver   s_buttons(Fuchey::Buttons::PIN_CONFIRM,
                                        Fuchey::Buttons::PIN_MENU,
                                        Fuchey::Buttons::PIN_SELECT,
                                        Fuchey::Buttons::PIN_BACK,
                                        Fuchey::Buttons::DEBOUNCE_MS,
                                        Fuchey::Buttons::LONG_PRESS_MS);
Fuchey::WalletCore            s_wallet_core;
namespace Fuchey { WalletCore* g_wallet_core_ptr = nullptr; }
static Fuchey::SpendingPolicy s_spending_policy;
static Fuchey::WalletManager  s_wallet_manager(s_wallet_core, s_spending_policy);

static Fuchey::WiFiManager    s_wifi_manager;
static Fuchey::AIManager      s_ai_manager(s_wifi_manager);
static Fuchey::WeatherService s_weather_service(s_wifi_manager);
static Fuchey::PriceService   s_price_service(s_wifi_manager);

// ─── Network State ─────────────────────────────────────────
static bool s_is_devnet = true; // Default to devnet for prototyping

static const char* get_rpc_url() {
    return s_is_devnet ? Fuchey::API::SOLANA_DEVNET_RPC : Fuchey::API::SOLANA_MAINNET_RPC;
}

static const char* get_usdc_mint() {
    return s_is_devnet ? Fuchey::API::USDC_DEVNET_MINT : Fuchey::API::USDC_MAINNET_MINT;
}

static Fuchey::BalanceMonitor s_balance_monitor(s_wifi_manager, "", get_usdc_mint(), get_rpc_url());

// ─── Helpers ──────────────────────────────────────────────

// Check if string is a Solana hex private key: 32-byte seed or 64-byte seed+pubkey.
static bool is_hex_private_key(const char* s) {
    size_t len = strlen(s);
    if (len != 64 && len != 128) return false;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool is_single_base58_token(const char* s) {
    if (!s || !*s) return false;
    for (const char* p = s; *p; ++p) {
        if (*p == ' ' || *p == '\t') return false;
        if (!std::strchr(Fuchey::Crypto::Base58::ALPHABET, *p)) return false;
    }
    return true;
}

static int count_bip39_words(const char* s) {
    int count = 0;
    while (s && *s) {
        while (*s && !std::isalpha(static_cast<unsigned char>(*s))) ++s;
        if (*s) {
            ++count;
            while (*s && std::isalpha(static_cast<unsigned char>(*s))) ++s;
        }
    }
    return count;
}

static std::string normalize_bip39_payload(const char* s) {
    std::string normalized;
    while (s && *s) {
        while (*s && !std::isalpha(static_cast<unsigned char>(*s))) ++s;
        if (!*s) break;

        if (!normalized.empty()) normalized += ' ';
        while (*s && std::isalpha(static_cast<unsigned char>(*s))) {
            normalized += static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
            ++s;
        }
    }
    return normalized;
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  Fuchey Firmware v%s", Fuchey::FW_VERSION);
    ESP_LOGI(TAG, "  Target: ESP32-S3 (16MB Flash, 8MB OPI PSRAM)");
    ESP_LOGI(TAG, "=================================================");

    // 1. Allocate Queues & Event Groups
    Fuchey::Events::g_wallet_queue = xQueueCreate(Fuchey::Queues::WALLET_REQUESTS, sizeof(Fuchey::Events::Event));
    Fuchey::Events::g_ui_queue     = xQueueCreate(Fuchey::Queues::UI_COMMANDS,     sizeof(Fuchey::Events::Event));
    Fuchey::Events::g_ai_queue     = xQueueCreate(Fuchey::Queues::AI_MESSAGES,     sizeof(Fuchey::Events::Event));
    Fuchey::Events::g_button_queue = xQueueCreate(Fuchey::Queues::BUTTON_EVENTS,   sizeof(Fuchey::ButtonState));
    Fuchey::Events::g_event_group  = xEventGroupCreate();

    g_button_queue_ref = Fuchey::Events::g_button_queue;
    Fuchey::g_tx_confirm_queue = xQueueCreate(4, sizeof(Fuchey::Events::Event));

    // 2. Initialize System Layer (NVS, Storage, Drivers)
    ESP_ERROR_CHECK(Fuchey::Storage::init());
    ESP_LOGI(TAG, "[OK] Storage (NVS) initialized");

    // Load network setting from NVS (default devnet)
    {
        Fuchey::Storage::Handle cfg(Fuchey::NVS::CONFIG_NS, NVS_READONLY);
        if (cfg.is_open()) {
            auto net = cfg.get_str(Fuchey::NVS::KEY_NETWORK);
            if (net && *net == "mainnet") {
                s_is_devnet = false;
            }
        }
    }
    ESP_LOGI(TAG, "[OK] Network configured: %s", s_is_devnet ? "Solana Devnet" : "Solana Mainnet-Beta");

    if (!s_display.init()) {
        ESP_LOGE(TAG, "[!!] Display initialization failed — continuing without OLED");
    } else {
        ESP_LOGI(TAG, "[OK] Display initialized");
    }
    s_ui.init();

    if (!s_buttons.init(Fuchey::Events::g_button_queue)) {
        ESP_LOGE(TAG, "[!!] Button Driver initialization failed");
    } else {
        ESP_LOGI(TAG, "[OK] Button driver initialized "
                      "(CONFIRM=GPIO%d, MENU=GPIO%d, SELECT=GPIO%d, BACK=GPIO%d)",
                 Fuchey::Buttons::PIN_CONFIRM, Fuchey::Buttons::PIN_MENU,
                 Fuchey::Buttons::PIN_SELECT, Fuchey::Buttons::PIN_BACK);
    }

    Fuchey::g_wallet_core_ptr = &s_wallet_core;
    s_spending_policy.init();
    s_wallet_core.init();
    s_wallet_manager.init();
    ESP_LOGI(TAG, "[OK] Wallet core initialized — state: %s",
             s_wallet_core.has_wallet() ? "LOCKED (wallet found)" : "UNINITIALIZED (no wallet)");

    if (s_wallet_core.has_wallet()) {
        auto addr_opt = s_wallet_core.get_address();
        if (addr_opt) {
            s_balance_monitor.set_address(*addr_opt);
        }
    }

    // 4. Initialize Network & Services
    s_wifi_manager.init();
    s_ai_manager.init();
    s_weather_service.init();
    s_price_service.init();
    ESP_LOGI(TAG, "[OK] Network services initialized");

    // Attempt WiFi auto-connect from NVS
    s_wifi_manager.connect_from_nvs();

    // Initialize NTP (auto-syncs after WiFi gets IP)
    setenv("TZ", "NPT-5:45", 1);  // Nepal Time (UTC+5:45)
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Boot animation — WiFi is already connecting in the background
    s_display.animate_boot(3000);

    // 5. Detect first-boot state for UIManager setup screen
    {
        bool wifi_missing = !s_wifi_manager.has_credentials();
        bool wallet_missing = !s_wallet_core.has_wallet();
        s_ui.set_setup_needed(wifi_missing, wallet_missing);

        ESP_LOGI(TAG, "-------------------------------------------------");
        ESP_LOGI(TAG, "  Boot State:");
        ESP_LOGI(TAG, "    WiFi credentials : %s", wifi_missing   ? "MISSING" : "SAVED");
        ESP_LOGI(TAG, "    Wallet           : %s", wallet_missing ? "MISSING" : "FOUND");
        ESP_LOGI(TAG, "-------------------------------------------------");
    }

    // 6. Spawn FreeRTOS Tasks with Strict CPU Core Pinning & Priorities
    ESP_LOGI(TAG, "Spawning FreeRTOS tasks...");

    // UI Task (Core 0)
    xTaskCreatePinnedToCore(Fuchey::UIManager::task_entry, "ui_task",
                            Fuchey::Tasks::UI_STACK, &s_ui,
                            Fuchey::Tasks::UI_PRIORITY, nullptr, Fuchey::Tasks::UI_CORE);

    // Wallet Manager Task (Core 1 — Security Critical)
    xTaskCreatePinnedToCore(Fuchey::WalletManager::task_entry, "wallet_mgr_task",
                            Fuchey::Tasks::WALLET_STACK, &s_wallet_manager,
                            Fuchey::Tasks::WALLET_PRIORITY, nullptr, Fuchey::Tasks::WALLET_CORE);

    // AI Manager Task (Core 0 — Untrusted)
    xTaskCreatePinnedToCore(Fuchey::AIManager::task_entry, "ai_task",
                            Fuchey::Tasks::AI_STACK, &s_ai_manager,
                            Fuchey::Tasks::AI_PRIORITY, nullptr, Fuchey::Tasks::AI_CORE);

    // Weather Task (Core 1 — TLS won't starve IDLE0 on CPU 0)
    xTaskCreatePinnedToCore(Fuchey::WeatherService::task_entry, "weather_task",
                            Fuchey::Tasks::WEATHER_STACK, &s_weather_service,
                            Fuchey::Tasks::WEATHER_PRIORITY, nullptr, Fuchey::Tasks::WEATHER_CORE);

    // Price Task (Core 1 — TLS won't starve IDLE0 on CPU 0)
    xTaskCreatePinnedToCore(Fuchey::PriceService::task_entry, "price_task",
                             Fuchey::Tasks::PRICE_STACK, &s_price_service,
                             Fuchey::Tasks::PRICE_PRIORITY, nullptr, Fuchey::Tasks::PRICE_CORE);

    // Wire BalanceMonitor to UIManager for on-demand balance fetch
    s_ui.set_balance_monitor(&s_balance_monitor);

    // Interactive Serial Console Task (Core 0)
    xTaskCreatePinnedToCore([](void*) {
        static constexpr const char* CTAG = "Console";

        ESP_LOGI(CTAG, "=================================================");
        ESP_LOGI(CTAG, "  Fuchey Interactive Serial Console Ready");
        ESP_LOGI(CTAG, "=================================================");
        ESP_LOGI(CTAG, "  Commands:");
        ESP_LOGI(CTAG, "    w <ssid> <pass>           WiFi connect & save");
        ESP_LOGI(CTAG, "    wallet_create              Generate new wallet");
        ESP_LOGI(CTAG, "    wallet_import <mnemonic>   Import BIP39 mnemonic");
        ESP_LOGI(CTAG, "    wallet_import <key>        Import hex/base58 private key");
        ESP_LOGI(CTAG, "    wallet_selftest            Verify Ed25519 math");
        ESP_LOGI(CTAG, "    wallet_info                Show current address");
        ESP_LOGI(CTAG, "    p                          Force SOL price fetch");
        ESP_LOGI(CTAG, "    c / 1                      CONFIRM press (tx accept)");
        ESP_LOGI(CTAG, "    x / 3                      CONFIRM double-press (tx reject)");
        ESP_LOGI(CTAG, "    l                          CONFIRM long-press (tx reject)");
        ESP_LOGI(CTAG, "    n / next                   MENU press (open menu / next)");
        ESP_LOGI(CTAG, "    q                          MENU double-press (show QR)");
        ESP_LOGI(CTAG, "    m / select                 SELECT press (choose option)");
        ESP_LOGI(CTAG, "    b / 2                      BACK button");
        ESP_LOGI(CTAG, "    h / ?                      Show this help");
        ESP_LOGI(CTAG, "=================================================");

        char line[512];
        std::string pending_line;
        std::string pending_wallet_import;
        while (true) {
            if (fgets(line, sizeof(line), stdin)) {
                size_t chunk_len = strlen(line);
                bool line_complete = chunk_len > 0 &&
                                     (line[chunk_len - 1] == '\r' || line[chunk_len - 1] == '\n');

                pending_line.append(line, chunk_len);
                if (!line_complete && pending_line.size() < sizeof(line) - 1) {
                    continue;
                }
                if (pending_line.size() >= sizeof(line)) {
                    ESP_LOGW(CTAG, "Command too long; discarding input (%d bytes)",
                             static_cast<int>(pending_line.size()));
                    pending_line.clear();
                    continue;
                }

                std::strncpy(line, pending_line.c_str(), sizeof(line));
                line[sizeof(line) - 1] = '\0';
                pending_line.clear();

                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
                    line[--len] = '\0';
                }
                // Trim leading whitespace
                char* cmd = line;
                while (*cmd == ' ' || *cmd == '\t') ++cmd;
                len = strlen(cmd);
                while (len > 0 && (cmd[len - 1] == ' ' || cmd[len - 1] == '\t')) {
                    cmd[--len] = '\0';
                }
                if (len == 0) continue;

                if (!pending_wallet_import.empty() &&
                    strcmp(cmd, "wallet_import_cancel") != 0) {
                    const char* continuation = cmd;
                    if (strncmp(cmd, "wallet_import ", 14) == 0 && len > 14) {
                        continuation = cmd + 14;
                        while (*continuation == ' ' || *continuation == '\t') ++continuation;
                    }
                    pending_wallet_import += ' ';
                    pending_wallet_import += continuation;
                    std::strncpy(line, pending_wallet_import.c_str(), sizeof(line));
                    line[sizeof(line) - 1] = '\0';
                    cmd = line;
                    len = strlen(cmd);
                }

                if (strcmp(cmd, "wallet_import_cancel") == 0) {
                    std::fill(pending_wallet_import.begin(), pending_wallet_import.end(), '\0');
                    pending_wallet_import.clear();
                    ESP_LOGW(CTAG, "[Wallet] Pending import cancelled");
                    continue;
                }

                // ── CONFIRM / SELECT button ────────────────────
                if ((cmd[0] == 'c' || cmd[0] == '1') && len == 1) {
                    ESP_LOGI(CTAG, "[INPUT] CONFIRM press (tx accept)");
                    Fuchey::ButtonState state{
                        .id = Fuchey::ButtonId::CONFIRM,
                        .event = Fuchey::ButtonEvent::PRESS,
                        .timestamp_ms = 0
                    };
                    xQueueSend(::g_button_queue_ref, &state, 0);

                // ── CONFIRM double-press (tx reject) ───────────
                } else if ((cmd[0] == 'x' || cmd[0] == '3') && len == 1) {
                    ESP_LOGI(CTAG, "[INPUT] CONFIRM double-press (tx reject)");
                    Fuchey::ButtonState state{
                        .id = Fuchey::ButtonId::CONFIRM,
                        .event = Fuchey::ButtonEvent::DOUBLE_PRESS,
                        .timestamp_ms = 0
                    };
                    xQueueSend(::g_button_queue_ref, &state, 0);

                // ── CONFIRM long-press (tx reject) ─────────────
                } else if (cmd[0] == 'l' && len == 1) {
                    ESP_LOGI(CTAG, "[INPUT] CONFIRM long-press (tx reject)");
                    Fuchey::ButtonState state{
                        .id = Fuchey::ButtonId::CONFIRM,
                        .event = Fuchey::ButtonEvent::LONG_PRESS,
                        .timestamp_ms = 0
                    };
                    xQueueSend(::g_button_queue_ref, &state, 0);

                // ── MENU button (open menu / next) ─────────────
                } else if (strcmp(cmd, "n") == 0 || strcmp(cmd, "next") == 0) {
                    ESP_LOGI(CTAG, "[INPUT] MENU press (open menu / next)");
                    Fuchey::ButtonState state{
                        .id = Fuchey::ButtonId::MENU,
                        .event = Fuchey::ButtonEvent::PRESS,
                        .timestamp_ms = 0
                    };
                    xQueueSend(::g_button_queue_ref, &state, 0);

                // ── MENU double-press (show QR) ────────────────
                } else if (strcmp(cmd, "q") == 0) {
                    ESP_LOGI(CTAG, "[INPUT] MENU double-press (show QR)");
                    Fuchey::ButtonState state{
                        .id = Fuchey::ButtonId::MENU,
                        .event = Fuchey::ButtonEvent::DOUBLE_PRESS,
                        .timestamp_ms = 0
                    };
                    xQueueSend(::g_button_queue_ref, &state, 0);

                // ── SELECT button (choose option) ──────────────
                } else if ((cmd[0] == 'm' && len == 1) || strcmp(cmd, "select") == 0) {
                    ESP_LOGI(CTAG, "[INPUT] SELECT press (choose option)");
                    Fuchey::ButtonState state{
                        .id = Fuchey::ButtonId::SELECT,
                        .event = Fuchey::ButtonEvent::PRESS,
                        .timestamp_ms = 0
                    };
                    xQueueSend(::g_button_queue_ref, &state, 0);

                // ── BACK button ───────────────────────────────
                } else if ((cmd[0] == 'b' || cmd[0] == '2') && len == 1) {
                    ESP_LOGI(CTAG, "[INPUT] BACK");
                    Fuchey::ButtonState state{
                        .id = Fuchey::ButtonId::BACK,
                        .event = Fuchey::ButtonEvent::PRESS,
                        .timestamp_ms = 0
                    };
                    xQueueSend(::g_button_queue_ref, &state, 0);

                // ── menu command ──────────────────────────────
                } else if (strcmp(cmd, "menu") == 0) {
                    ESP_LOGI(CTAG, "[Console] Opening Main Menu on OLED screen");
                    s_ui.set_screen(Fuchey::UIScreen::MENU_MAIN);

                // ── WiFi connect ──────────────────────────────
                } else if (cmd[0] == 'w' && cmd[1] == ' ' && len > 2) {
                    char ssid[64] = {0}, pass[64] = {0};
                    int parsed = sscanf(cmd + 2, "%63s %63s", ssid, pass);
                    if (parsed >= 1) {
                        ESP_LOGI(CTAG, "-------------------------------------------------");
                        ESP_LOGI(CTAG, "[WiFi] Saving credentials and connecting...");
                        ESP_LOGI(CTAG, "       SSID: %s", ssid);
                        ESP_LOGI(CTAG, "-------------------------------------------------");
                        s_wifi_manager.save_credentials(ssid, pass);
                        s_wifi_manager.connect(ssid, pass);
                        s_ui.mark_wifi_configured(ssid);
                    } else {
                        ESP_LOGW(CTAG, "Usage: w <SSID> <PASSWORD>");
                    }

                // ── Force SOL price update ────────────────────
                } else if (cmd[0] == 'p' && len == 1) {
                    ESP_LOGI(CTAG, "[Input] Fetching live SOL price...");
                    s_price_service.update_now();

                // ── wallet_info ───────────────────────────────
                } else if (strcmp(cmd, "wallet_info") == 0) {
                    auto addr = s_wallet_core.get_address();
                    if (addr) {
                        ESP_LOGI(CTAG, "-------------------------------------------------");
                        ESP_LOGI(CTAG, "  Wallet Address: %s", addr->c_str());
                        ESP_LOGI(CTAG, "  State: %s",
                                 s_wallet_core.is_unlocked() ? "UNLOCKED" : "LOCKED");
                        ESP_LOGI(CTAG, "-------------------------------------------------");
                    } else {
                        ESP_LOGW(CTAG, "  No wallet configured yet.");
                    }

                // ── wallet_selftest ───────────────────────────
                } else if (strcmp(cmd, "wallet_selftest") == 0) {
                    ESP_LOGI(CTAG, "-------------------------------------------------");
                    ESP_LOGI(CTAG, "[Wallet] Running Ed25519 RFC8032 self-test...");
                    if (Fuchey::Crypto::Ed25519::self_test()) {
                        ESP_LOGI(CTAG, "[Wallet] Self-test PASSED");
                    } else {
                        ESP_LOGE(CTAG, "[Wallet] Self-test FAILED");
                    }
                    ESP_LOGI(CTAG, "-------------------------------------------------");

                // ── qr ────────────────────────────────────────
                } else if (strcmp(cmd, "qr") == 0) {
                    ESP_LOGI(CTAG, "[UI] Switching to Wallet QR screen");
                    s_ui.set_screen(Fuchey::UIScreen::WALLET_QR);

                // ── balance ───────────────────────────────────
                } else if (strcmp(cmd, "balance") == 0) {
                    auto addr = s_wallet_core.get_address();
                    if (!addr) {
                        ESP_LOGW(CTAG, "No wallet configured yet.");
                    } else if (!s_wifi_manager.has_ip()) {
                        ESP_LOGW(CTAG, "WiFi not connected — cannot fetch balance.");
                    } else {
                        xTaskCreate([](void*) {
                            static constexpr const char* CTAG = "Console";
                            auto addr_opt = s_wallet_core.get_address();
                            if (!addr_opt) { vTaskDelete(nullptr); return; }
                            std::string address = *addr_opt;

                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            ESP_LOGI(CTAG, "[Balance] Querying %s (%s) for %s...",
                                     s_is_devnet ? "Devnet" : "Mainnet-Beta",
                                     get_rpc_url(),
                                     address.c_str());

                            // ── Fetch SOL balance ──────────────────────────
                            char sol_req[256];
                            snprintf(sol_req, sizeof(sol_req),
                                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getBalance\",\"params\":[\"%s\"]}",
                                     address.c_str());

                            double sol_bal   = 0.0;
                            double lamports  = 0.0;
                            bool   sol_ok    = false;

                            auto resp = s_wifi_manager.post_json(get_rpc_url(), sol_req);
                            if (resp.success) {
                                cJSON* root = cJSON_Parse(resp.body.c_str());
                                if (root) {
                                    cJSON* res = cJSON_GetObjectItem(root, "result");
                                    cJSON* val = res ? cJSON_GetObjectItem(res, "value") : nullptr;
                                    if (val && cJSON_IsNumber(val)) {
                                        lamports = val->valuedouble;
                                        sol_bal  = lamports / 1000000000.0;
                                        sol_ok   = true;
                                    } else {
                                        cJSON* err_item = cJSON_GetObjectItem(root, "error");
                                        if (err_item) {
                                            cJSON* msg = cJSON_GetObjectItem(err_item, "message");
                                            ESP_LOGE(CTAG, "  SOL RPC Error: %s",
                                                     msg && msg->valuestring ? msg->valuestring : "Unknown error");
                                        }
                                    }
                                    cJSON_Delete(root);
                                } else {
                                    ESP_LOGE(CTAG, "  SOL RPC Parse Error. Raw: %.100s", resp.body.c_str());
                                }
                            } else {
                                ESP_LOGE(CTAG, "Failed to query SOL balance (HTTP %d)", resp.status_code);
                            }

                            // Small delay between RPC calls
                            vTaskDelay(pdMS_TO_TICKS(100));

                            // ── Fetch USDC balance ─────────────────────────
                            char usdc_req[512];
                            snprintf(usdc_req, sizeof(usdc_req),
                                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTokenAccountsByOwner\","
                                     "\"params\":[\"%s\",{\"mint\":\"%s\"},{\"encoding\":\"jsonParsed\"}]}",
                                     address.c_str(), get_usdc_mint());

                            double usdc_bal = 0.0;
                            bool   usdc_ok  = false;

                            auto u_resp = s_wifi_manager.post_json(get_rpc_url(), usdc_req);
                            if (u_resp.success) {
                                cJSON* root = cJSON_Parse(u_resp.body.c_str());
                                if (root) {
                                    cJSON* res = cJSON_GetObjectItem(root, "result");
                                    cJSON* val = res ? cJSON_GetObjectItem(res, "value") : nullptr;
                                    if (cJSON_IsArray(val) && cJSON_GetArraySize(val) > 0) {
                                        cJSON* item0   = cJSON_GetArrayItem(val, 0);
                                        cJSON* account = cJSON_GetObjectItem(item0, "account");
                                        cJSON* data    = account ? cJSON_GetObjectItem(account, "data")   : nullptr;
                                        cJSON* parsed  = data    ? cJSON_GetObjectItem(data,    "parsed") : nullptr;
                                        cJSON* info    = parsed  ? cJSON_GetObjectItem(parsed,  "info")   : nullptr;
                                        cJSON* t_amt   = info    ? cJSON_GetObjectItem(info,    "tokenAmount") : nullptr;
                                        cJSON* ui_amt  = t_amt   ? cJSON_GetObjectItem(t_amt,   "uiAmount")   : nullptr;
                                        if (ui_amt && cJSON_IsNumber(ui_amt)) {
                                            usdc_bal = ui_amt->valuedouble;
                                            usdc_ok  = true;
                                        }
                                    } else {
                                        usdc_ok = true; // account exists, just 0 balance
                                    }
                                    cJSON_Delete(root);
                                }
                            } else {
                                ESP_LOGE(CTAG, "Failed to query USDC balance (HTTP %d)", u_resp.status_code);
                            }

                            // ── Print both on one line ──────────────────────
                            ESP_LOGI(CTAG, "  SOL: %.6f SOL  |  USDC: $%.2f",
                                     sol_ok ? sol_bal : 0.0,
                                     usdc_ok ? usdc_bal : 0.0);
                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            vTaskDelete(nullptr);
                        }, "bal_task", 8192, nullptr, 4, nullptr);
                    }

                // ── network ───────────────────────────────────
                } else if (strcmp(cmd, "network") == 0) {
                    ESP_LOGI(CTAG, "=================================================");
                    ESP_LOGI(CTAG, "  Solana Network: %s", s_is_devnet ? "DEVNET" : "MAINNET-BETA");
                    ESP_LOGI(CTAG, "  RPC Endpoint:   %s", get_rpc_url());
                    ESP_LOGI(CTAG, "  USDC Mint:      %s", get_usdc_mint());
                    ESP_LOGI(CTAG, "  Commands:");
                    ESP_LOGI(CTAG, "    network devnet   - Switch to Solana Devnet");
                    ESP_LOGI(CTAG, "    network mainnet  - Switch to Solana Mainnet");
                    ESP_LOGI(CTAG, "=================================================");

                } else if (strcmp(cmd, "network devnet") == 0) {
                    s_is_devnet = true;
                    Fuchey::Storage::Handle cfg(Fuchey::NVS::CONFIG_NS, NVS_READWRITE);
                    if (cfg.is_open()) {
                        cfg.set_str(Fuchey::NVS::KEY_NETWORK, "devnet");
                        cfg.commit();
                    }
                    ESP_LOGI(CTAG, "-------------------------------------------------");
                    ESP_LOGI(CTAG, "[Network] Switched to Solana DEVNET");
                    ESP_LOGI(CTAG, "  RPC: %s", get_rpc_url());
                    ESP_LOGI(CTAG, "-------------------------------------------------");

                } else if (strcmp(cmd, "network mainnet") == 0) {
                    s_is_devnet = false;
                    Fuchey::Storage::Handle cfg(Fuchey::NVS::CONFIG_NS, NVS_READWRITE);
                    if (cfg.is_open()) {
                        cfg.set_str(Fuchey::NVS::KEY_NETWORK, "mainnet");
                        cfg.commit();
                    }
                    ESP_LOGI(CTAG, "-------------------------------------------------");
                    ESP_LOGI(CTAG, "[Network] Switched to Solana MAINNET-BETA");
                    ESP_LOGI(CTAG, "  RPC: %s", get_rpc_url());
                    ESP_LOGI(CTAG, "-------------------------------------------------");

                // ── Devnet Airdrop ────────────────────────────
                } else if (strncmp(cmd, "airdrop", 7) == 0) {
                    auto addr = s_wallet_core.get_address();
                    if (!addr) {
                        ESP_LOGW(CTAG, "No wallet configured yet.");
                    } else if (!s_is_devnet) {
                        ESP_LOGW(CTAG, "Airdrop is only available on Devnet! Type 'network devnet' first.");
                    } else if (!s_wifi_manager.has_ip()) {
                        ESP_LOGW(CTAG, "WiFi not connected.");
                    } else {
                        float sol_amt = 1.0f;
                        sscanf(cmd + 7, "%f", &sol_amt);
                        if (sol_amt <= 0.0f) sol_amt = 1.0f;

                        xTaskCreate([](void* arg) {
                            static constexpr const char* CTAG = "Console";
                            float amount = *static_cast<float*>(arg);
                            delete static_cast<float*>(arg);

                            auto addr_opt = s_wallet_core.get_address();
                            if (!addr_opt) { vTaskDelete(nullptr); return; }
                            std::string address = *addr_opt;

                            uint64_t lamports = static_cast<uint64_t>(amount * 1000000000.0f);
                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            ESP_LOGI(CTAG, "[Airdrop] Requesting %.1f Devnet SOL for %s...", amount, address.c_str());

                            char req[384];
                            snprintf(req, sizeof(req),
                                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"requestAirdrop\",\"params\":[\"%s\",%llu]}",
                                     address.c_str(), lamports);

                            auto resp = s_wifi_manager.post_json(get_rpc_url(), req);
                            if (resp.success) {
                                cJSON* root = cJSON_Parse(resp.body.c_str());
                                cJSON* res = root ? cJSON_GetObjectItem(root, "result") : nullptr;
                                if (res && cJSON_IsString(res)) {
                                    ESP_LOGI(CTAG, "  Airdrop SUCCESS! Tx Signature:");
                                    ESP_LOGI(CTAG, "  %s", res->valuestring);
                                } else {
                                    ESP_LOGW(CTAG, "  Airdrop requested (response received)");
                                }
                                if (root) cJSON_Delete(root);
                            } else {
                                ESP_LOGE(CTAG, "Airdrop request failed");
                            }
                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            vTaskDelete(nullptr);
                        }, "drop_task", 8192, new float(sol_amt), 4, nullptr);
                    }

                // ── wallet_create ─────────────────────────────
                } else if (strcmp(cmd, "wallet_create") == 0) {
                    ESP_LOGI(CTAG, "-------------------------------------------------");
                    ESP_LOGI(CTAG, "[Wallet] Generating new 12-word wallet...");
                    std::string mnemonic;
                    Fuchey::WalletResult r = s_wallet_core.create(12, mnemonic);
                    if (r == Fuchey::WalletResult::OK) {
                        auto addr = s_wallet_core.get_address();
                        ESP_LOGI(CTAG, "[Wallet] CREATED SUCCESSFULLY!");
                        ESP_LOGI(CTAG, "  Address: %s", addr ? addr->c_str() : "(error)");
                        ESP_LOGI(CTAG, "  ------- BACKUP YOUR SEED PHRASE -------");
                        ESP_LOGI(CTAG, "  %s", mnemonic.c_str());
                        ESP_LOGI(CTAG, "  ----------------------------------------");
                        ESP_LOGI(CTAG, "  WARNING: Save these 12 words securely!");
                        ESP_LOGI(CTAG, "  They will NOT be shown again.");
                        // Zero mnemonic from memory after logging
                        memset(&mnemonic[0], 0, mnemonic.size());
                        mnemonic.clear();

                        std::string addr_str = addr ? *addr : "";
                        Fuchey::Events::Event evt{};
                        evt.type = Fuchey::Events::EventType::WALLET_CREATED;
                        Fuchey::Events::post(Fuchey::Events::g_wallet_queue, evt);
                        s_ui.mark_wallet_configured(addr_str.c_str());
                        s_balance_monitor.set_address(addr_str);
                    } else if (r == Fuchey::WalletResult::ERR_ALREADY_EXISTS) {
                        ESP_LOGW(CTAG, "[Wallet] A wallet already exists!");
                        ESP_LOGW(CTAG, "  Use 'wallet_reset' first to erase it, then 'wallet_create'.");
                        auto addr = s_wallet_core.get_address();
                        ESP_LOGW(CTAG, "  Current address: %s", addr ? addr->c_str() : "(unknown)");
                    } else {
                        ESP_LOGE(CTAG, "[Wallet] Creation FAILED (err=%d)", static_cast<int>(r));
                    }
                    ESP_LOGI(CTAG, "-------------------------------------------------");

                // ── wallet_reset ──────────────────────────────
                } else if (strcmp(cmd, "wallet_reset") == 0) {
                    ESP_LOGW(CTAG, "-------------------------------------------------");
                    ESP_LOGW(CTAG, "[Wallet] FACTORY RESET — erasing wallet from NVS...");
                    Fuchey::WalletResult r = s_wallet_core.factory_reset();
                    if (r == Fuchey::WalletResult::OK) {
                        ESP_LOGW(CTAG, "[Wallet] Reset complete. You can now run 'wallet_create'.");
                    } else {
                        ESP_LOGE(CTAG, "[Wallet] Reset FAILED (err=%d)", static_cast<int>(r));
                    }
                    ESP_LOGW(CTAG, "-------------------------------------------------");


                // ── wallet_import ─────────────────────────────
                } else if (strncmp(cmd, "wallet_import ", 14) == 0 && len > 14) {
                    const char* payload = cmd + 14;
                    // Skip leading spaces
                    while (*payload == ' ' || *payload == '\t') ++payload;

                    ESP_LOGI(CTAG, "-------------------------------------------------");
                    Fuchey::WalletResult r;

                    // Auto-detect format: hex, Solana base58 secret key, or BIP39.
                    if (is_hex_private_key(payload)) {
                        ESP_LOGI(CTAG, "[Wallet] Detected: %d-char hex private key", static_cast<int>(strlen(payload)));
                        r = s_wallet_core.import_privkey_hex(payload);
                    } else if (is_single_base58_token(payload)) {
                        size_t payload_len = strlen(payload);
                        if (payload_len < 87) {
                            pending_wallet_import = "wallet_import ";
                            pending_wallet_import += payload;
                            ESP_LOGW(CTAG, "[Wallet] Private key input looks incomplete (%d chars). Paste/type the remaining characters, or wallet_import_cancel.",
                                     static_cast<int>(payload_len));
                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            continue;
                        }
                        ESP_LOGI(CTAG, "[Wallet] Detected: base58 private key");
                        r = s_wallet_core.import_privkey_base58(payload);
                    } else {
                        std::string normalized = normalize_bip39_payload(payload);
                        int word_count = count_bip39_words(normalized.c_str());
                        ESP_LOGI(CTAG, "[Wallet] Detected: BIP39 mnemonic (%d words)", word_count);
                        ESP_LOGI(CTAG, "[Wallet] Mnemonic payload length: raw=%d normalized=%d",
                                 static_cast<int>(strlen(payload)),
                                 static_cast<int>(normalized.size()));
                        if (word_count > 0 && word_count < 12) {
                            pending_wallet_import = "wallet_import ";
                            pending_wallet_import += normalized;
                            ESP_LOGW(CTAG, "[Wallet] Mnemonic input is incomplete (%d/12 words). Paste/type the remaining word(s), or wallet_import_cancel.",
                                     word_count);
                            std::fill(normalized.begin(), normalized.end(), '\0');
                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            continue;
                        }
                        if (word_count > 12 && word_count < 24) {
                            pending_wallet_import = "wallet_import ";
                            pending_wallet_import += normalized;
                            ESP_LOGW(CTAG, "[Wallet] Mnemonic input is incomplete (%d/24 words). Paste/type the remaining word(s), or wallet_import_cancel.",
                                     word_count);
                            std::fill(normalized.begin(), normalized.end(), '\0');
                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            continue;
                        }
                        r = s_wallet_core.import(normalized);
                        std::fill(normalized.begin(), normalized.end(), '\0');
                    }

                    if (r == Fuchey::WalletResult::OK) {
                        std::fill(pending_wallet_import.begin(), pending_wallet_import.end(), '\0');
                        pending_wallet_import.clear();
                        auto addr = s_wallet_core.get_address();
                        ESP_LOGI(CTAG, "[Wallet] IMPORTED SUCCESSFULLY!");
                        ESP_LOGI(CTAG, "  Address: %s", addr ? addr->c_str() : "(error)");

                        std::string addr_str = addr ? *addr : "";
                        Fuchey::Events::Event evt{};
                        evt.type = Fuchey::Events::EventType::WALLET_IMPORTED;
                        Fuchey::Events::post(Fuchey::Events::g_wallet_queue, evt);
                        s_ui.mark_wallet_configured(addr_str.c_str());
                        s_balance_monitor.set_address(addr_str);
                    } else {
                        const char* reason =
                            (r == Fuchey::WalletResult::ERR_INVALID_MNEMONIC) ? "invalid mnemonic" :
                            (r == Fuchey::WalletResult::ERR_INVALID_PRIVKEY)  ? "invalid private key (bad hex/base58?)" :
                            (r == Fuchey::WalletResult::ERR_ALREADY_EXISTS)   ? "wallet already exists" :
                            "unknown error";
                        ESP_LOGE(CTAG, "[Wallet] Import FAILED: %s", reason);
                    }

                    // Zero the input line for security
                    memset(cmd + 14, 0, len - 14);
                    ESP_LOGI(CTAG, "-------------------------------------------------");

                // ── AI chat message ────────────────────────────
                } else if (strncmp(cmd, "ai ", 3) == 0 && len > 3) {
                    const char* msg = cmd + 3;
                    ESP_LOGI(CTAG, "[AI] Sending message to AI Assistant: '%s'", msg);
                    Fuchey::Events::Event evt{};
                    evt.type = Fuchey::Events::EventType::AI_MESSAGE_RECV;
                    std::strncpy(evt.data.chat.text, msg, sizeof(evt.data.chat.text) - 1);
                    Fuchey::Events::post(Fuchey::Events::g_ai_queue, evt);

                // ── AI API key ─────────────────────────────────
                } else if (strncmp(cmd, "ai_key ", 7) == 0 && len > 7) {
                    const char* key = cmd + 7;
                    if (s_ai_manager.set_api_key(key)) {
                        ESP_LOGI(CTAG, "[AI] API Key saved to NVS successfully");
                    } else {
                        ESP_LOGE(CTAG, "[AI] Failed to save API Key to NVS");
                    }

                // ── Send SOL ──────────────────────────────────
                // ── Send SOL ──────────────────────────────────
                } else if (strncmp(cmd, "send sol ", 9) == 0 && len > 9) {
                    struct SendArgs {
                        float amount;
                        char  recipient[64];
                    };

                    auto* args = new SendArgs();
                    args->amount = 0.0f;
                    memset(args->recipient, 0, sizeof(args->recipient));

                    if (sscanf(cmd + 9, "%f %63s", &args->amount, args->recipient) == 2) {
                        xTaskCreate([](void* p) {
                            static constexpr const char* CTAG = "Console";
                            auto* args = static_cast<SendArgs*>(p);
                            float amount = args->amount;
                            std::string recipient_str = args->recipient;
                            delete args;

                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            ESP_LOGI(CTAG, "[SEND SOL] Initiating transfer:");
                            ESP_LOGI(CTAG, "  Asset:     SOL");
                            ESP_LOGI(CTAG, "  Amount:    %.4f SOL", amount);
                            ESP_LOGI(CTAG, "  Recipient: %s", recipient_str.c_str());

                            // Check recipient address validity
                            auto recipient_bytes = Fuchey::Crypto::Base58::decode(recipient_str);
                            if (recipient_bytes.size() != 32) {
                                ESP_LOGE(CTAG, "[SEND SOL] Error: Invalid Solana recipient address length (%d bytes, expected 32).",
                                         recipient_bytes.size());
                                ESP_LOGI(CTAG, "-------------------------------------------------");
                                vTaskDelete(nullptr);
                                return;
                            }

                            s_wallet_core.unlock();
                            auto pubkey_opt = s_wallet_core.get_pubkey();
                            if (!pubkey_opt) {
                                ESP_LOGE(CTAG, "[SEND SOL] Error: Wallet is locked or not setup.");
                                ESP_LOGI(CTAG, "-------------------------------------------------");
                                vTaskDelete(nullptr);
                                return;
                            }
                            auto sender_pubkey = *pubkey_opt;

                            // Flush any old events from g_tx_confirm_queue
                            Fuchey::Events::Event dummy_evt;
                            if (Fuchey::g_tx_confirm_queue) {
                                while (xQueueReceive(Fuchey::g_tx_confirm_queue, &dummy_evt, 0) == pdTRUE) {}
                            }

                            // Prompt UI for hardware/serial button approval ($150/SOL baseline estimate)
                            uint64_t cents = static_cast<uint64_t>(amount * 150.0f * 100.0f);
                            Fuchey::Events::Event evt{};
                            evt.type = Fuchey::Events::EventType::TX_REQUEST;
                            evt.data.tx.amount_cents = cents;
                            evt.data.tx.tx_len = 0;
                            Fuchey::Events::post(Fuchey::Events::g_ui_queue, evt);

                            ESP_LOGI(CTAG, "[SEND SOL] Waiting for hardware button press or serial 'c' approval...");
                            ESP_LOGI(CTAG, "-------------------------------------------------");

                            // Wait up to 30 seconds for user confirmation on g_tx_confirm_queue
                            Fuchey::Events::Event app_evt{};
                            bool got_response = false;
                            if (Fuchey::g_tx_confirm_queue) {
                                got_response = (xQueueReceive(Fuchey::g_tx_confirm_queue, &app_evt, pdMS_TO_TICKS(30000)) == pdTRUE);
                            }

                            if (!got_response || app_evt.type != Fuchey::Events::EventType::TX_APPROVED) {
                                ESP_LOGW(CTAG, "-------------------------------------------------");
                                ESP_LOGW(CTAG, "[SEND SOL] Transfer REJECTED or TIMED OUT by user.");
                                ESP_LOGW(CTAG, "-------------------------------------------------");
                                vTaskDelete(nullptr);
                                return;
                            }

                            ESP_LOGI(CTAG, "[SEND SOL] Transaction APPROVED! Fetching blockhash from Devnet...");

                            // Fetch latest blockhash
                            auto bh_resp = s_wifi_manager.post_json(
                                get_rpc_url(),
                                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getLatestBlockhash\",\"params\":[{\"commitment\":\"finalized\"}]}"
                            );

                            if (!bh_resp.success) {
                                ESP_LOGE(CTAG, "[SEND SOL] Failed to get latest blockhash from RPC (status %d)", bh_resp.status_code);
                                vTaskDelete(nullptr);
                                return;
                            }

                            std::string blockhash_str;
                            cJSON* bh_root = cJSON_Parse(bh_resp.body.c_str());
                            if (bh_root) {
                                cJSON* res = cJSON_GetObjectItem(bh_root, "result");
                                cJSON* val = res ? cJSON_GetObjectItem(res, "value") : nullptr;
                                cJSON* bh  = val ? cJSON_GetObjectItem(val, "blockhash") : nullptr;
                                if (bh && bh->valuestring) {
                                    blockhash_str = bh->valuestring;
                                }
                                cJSON_Delete(bh_root);
                            }

                            if (blockhash_str.empty()) {
                                ESP_LOGE(CTAG, "[SEND SOL] Error parsing blockhash: %.100s", bh_resp.body.c_str());
                                vTaskDelete(nullptr);
                                return;
                            }

                            auto blockhash_bytes = Fuchey::Crypto::Base58::decode(blockhash_str);
                            if (blockhash_bytes.size() != 32) {
                                ESP_LOGE(CTAG, "[SEND SOL] Invalid blockhash decode size (%d)", blockhash_bytes.size());
                                vTaskDelete(nullptr);
                                return;
                            }

                            // ── Build Solana message (the bytes that get signed) ──
                            // Solana legacy message = header (3 bytes)
                            //   + compact-u16 account count + accounts
                            //   + recent blockhash (32 bytes)
                            //   + compact-u16 instruction count + instructions
                            // The num-signatures prefix (0x01) lives ONLY in the
                            // wire transaction, never in the signed message.
                            uint64_t lamports = static_cast<uint64_t>(amount * 1000000000.0f);
                            std::vector<uint8_t> msg;

                            // Header (3 bytes)
                            msg.push_back(1); // num_required_signatures = 1
                            msg.push_back(0); // num_readonly_signed_accounts = 0
                            msg.push_back(1); // num_readonly_unsigned_accounts = 1 (System Program)

                            // Account addresses (compact-u16 count + 32-byte keys)
                            msg.push_back(3); // 3 accounts: sender, recipient, System Program
                            msg.insert(msg.end(), sender_pubkey.begin(), sender_pubkey.end()); // [0] signer+writable
                            msg.insert(msg.end(), recipient_bytes.begin(), recipient_bytes.end()); // [1] writable
                            for (int i = 0; i < 32; i++) msg.push_back(0); // [2] System Program = 11111…

                            // Recent blockhash (32 bytes)
                            msg.insert(msg.end(), blockhash_bytes.begin(), blockhash_bytes.end());

                            // Instructions (compact-u16 count + instruction data)
                            msg.push_back(1); // 1 instruction
                            // Instruction: System Program Transfer
                            msg.push_back(2); // program_id_index = 2 (System Program)
                            msg.push_back(2); // 2 account indices follow
                            msg.push_back(0); // accounts[0] = sender  (index into account list)
                            msg.push_back(1); // accounts[1] = recipient
                            msg.push_back(12); // data length = 12 bytes
                            // Transfer instruction data: u32 discriminant=2 + u64 lamports LE
                            msg.push_back(2); msg.push_back(0); msg.push_back(0); msg.push_back(0);
                            for (int i = 0; i < 8; i++) {
                                msg.push_back(static_cast<uint8_t>((lamports >> (i * 8)) & 0xFF));
                            }

                            // Sign the message bytes
                            Fuchey::Crypto::Signature sig{};
                            auto sign_res = s_wallet_core.sign(msg, sig);
                            if (sign_res != Fuchey::WalletResult::OK) {
                                ESP_LOGE(CTAG, "[SEND SOL] Signing failed (err=%d)", static_cast<int>(sign_res));
                                vTaskDelete(nullptr);
                                return;
                            }

                            // ── Build wire transaction ──
                            // Format: [compact-u16 num_sigs] [sig…] [message]
                            std::vector<uint8_t> wire_tx;
                            wire_tx.push_back(1); // compact-u16: 1 signature
                            wire_tx.insert(wire_tx.end(), sig.begin(), sig.end()); // 64-byte signature
                            wire_tx.insert(wire_tx.end(), msg.begin(), msg.end()); // message bytes

                            std::string base58_tx = Fuchey::Crypto::Base58::encode(wire_tx);

                            char req_buf[2048];
                            snprintf(req_buf, sizeof(req_buf),
                                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"sendTransaction\",\"params\":[\"%s\",{\"encoding\":\"base58\"}]}",
                                     base58_tx.c_str());

                            ESP_LOGI(CTAG, "[SEND SOL] Broadcasting signed transaction to Solana Devnet...");
                            auto tx_resp = s_wifi_manager.post_json(get_rpc_url(), req_buf);

                            Fuchey::Events::Event result_evt{};
                            result_evt.data.tx.amount_cents = static_cast<uint64_t>(amount * 100.0f);
                            snprintf(reinterpret_cast<char*>(result_evt.data.tx.tx_data),
                                     sizeof(result_evt.data.tx.tx_data),
                                     "SOL:%.4f:%s", amount, recipient_str.c_str());

                            if (tx_resp.success) {
                                cJSON* tx_root = cJSON_Parse(tx_resp.body.c_str());
                                if (tx_root) {
                                    cJSON* tx_res = cJSON_GetObjectItem(tx_root, "result");
                                    cJSON* tx_err = cJSON_GetObjectItem(tx_root, "error");
                                    if (tx_res && tx_res->valuestring) {
                                        ESP_LOGI(CTAG, "=================================================");
                                        ESP_LOGI(CTAG, "  [SUCCESS] SOL Transfer Broadcast Complete!");
                                        ESP_LOGI(CTAG, "  Signature: %s", tx_res->valuestring);
                                        ESP_LOGI(CTAG, "  Explorer:  https://explorer.solana.com/tx/%s?cluster=devnet", tx_res->valuestring);
                                        ESP_LOGI(CTAG, "=================================================");
                                        result_evt.type = Fuchey::Events::EventType::TX_BROADCAST_OK;
                                    } else if (tx_err) {
                                        cJSON* msg_item = cJSON_GetObjectItem(tx_err, "message");
                                        const char* err_str = msg_item && msg_item->valuestring ? msg_item->valuestring : "Unknown RPC error";
                                        ESP_LOGE(CTAG, "  [TX ERROR] RPC Error: %s", err_str);
                                        snprintf(reinterpret_cast<char*>(result_evt.data.tx.tx_data),
                                                 sizeof(result_evt.data.tx.tx_data), "%s", err_str);
                                        result_evt.type = Fuchey::Events::EventType::TX_BROADCAST_FAIL;
                                    } else {
                                        ESP_LOGE(CTAG, "  [TX ERROR] Response: %.150s", tx_resp.body.c_str());
                                        snprintf(reinterpret_cast<char*>(result_evt.data.tx.tx_data),
                                                 sizeof(result_evt.data.tx.tx_data), "RPC parse error");
                                        result_evt.type = Fuchey::Events::EventType::TX_BROADCAST_FAIL;
                                    }
                                    cJSON_Delete(tx_root);
                                } else {
                                    ESP_LOGE(CTAG, "  [TX ERROR] Parse failure. Raw: %.150s", tx_resp.body.c_str());
                                    snprintf(reinterpret_cast<char*>(result_evt.data.tx.tx_data),
                                             sizeof(result_evt.data.tx.tx_data), "RPC parse failure");
                                    result_evt.type = Fuchey::Events::EventType::TX_BROADCAST_FAIL;
                                }
                            } else {
                                ESP_LOGE(CTAG, "  [TX ERROR] HTTP request failed (%d)", tx_resp.status_code);
                                snprintf(reinterpret_cast<char*>(result_evt.data.tx.tx_data),
                                         sizeof(result_evt.data.tx.tx_data), "HTTP %d", tx_resp.status_code);
                                result_evt.type = Fuchey::Events::EventType::TX_BROADCAST_FAIL;
                            }

                            Fuchey::Events::post(Fuchey::Events::g_ui_queue, result_evt);
                            vTaskDelete(nullptr);
                        }, "send_sol_task", 20480, args, 4, nullptr);
                    } else {
                        delete args;
                        ESP_LOGW(CTAG, "Usage: send sol <amount> <recipient_address>");
                    }

                // ── Send USDC ─────────────────────────────────
                } else if (strncmp(cmd, "send usdc ", 10) == 0 && len > 10) {
                    struct SendUsdcArgs {
                        float amount;
                        char  recipient[64];
                    };

                    auto* args = new SendUsdcArgs();
                    args->amount = 0.0f;
                    memset(args->recipient, 0, sizeof(args->recipient));

                    if (sscanf(cmd + 10, "%f %63s", &args->amount, args->recipient) == 2) {
                        xTaskCreate([](void* p) {
                            static constexpr const char* CTAG = "Console";
                            auto* args = static_cast<SendUsdcArgs*>(p);
                            float amount = args->amount;
                            std::string recipient_str = args->recipient;
                            delete args;

                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            ESP_LOGI(CTAG, "[SEND USDC] Initiating SPL transfer:");
                            ESP_LOGI(CTAG, "  Asset:     USDC (SPL Token)");
                            ESP_LOGI(CTAG, "  Amount:    $%.2f USDC", amount);
                            ESP_LOGI(CTAG, "  Recipient: %s", recipient_str.c_str());

                            // Decode recipient address
                            auto recipient_pubkey = Fuchey::Crypto::Base58::decode(recipient_str);
                            if (recipient_pubkey.size() != 32) {
                                ESP_LOGE(CTAG, "[SEND USDC] Error: Invalid Solana recipient address length (%d bytes, expected 32).",
                                         recipient_pubkey.size());
                                ESP_LOGI(CTAG, "-------------------------------------------------");
                                vTaskDelete(nullptr);
                                return;
                            }

                            s_wallet_core.unlock();
                            auto pubkey_opt = s_wallet_core.get_pubkey();
                            if (!pubkey_opt) {
                                ESP_LOGE(CTAG, "[SEND USDC] Error: Wallet is locked or not setup.");
                                ESP_LOGI(CTAG, "-------------------------------------------------");
                                vTaskDelete(nullptr);
                                return;
                            }
                            auto sender_pubkey = *pubkey_opt;

                            // ── Decode program IDs ──
                            auto token_prog = Fuchey::Crypto::Base58::decode(
                                "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA");
                            auto ata_prog = Fuchey::Crypto::Base58::decode(
                                "ATokenGPvbdGVxr1b2hvZbsiqW5xrj25vdTucN6s");
                            auto usdc_mint = Fuchey::Crypto::Base58::decode(get_usdc_mint());
                            if (token_prog.size() != 32 || ata_prog.size() != 32 || usdc_mint.size() != 32) {
                                ESP_LOGE(CTAG, "[SEND USDC] Error decoding program IDs");
                                vTaskDelete(nullptr);
                                return;
                            }

                            // ── Derive Associated Token Accounts ──
                            // ATA = first 32 bytes of SHA256(owner || token_prog || mint || 0xFF || ata_prog)
                            auto derive_ata = [&](const std::vector<uint8_t>& owner) -> std::array<uint8_t, 32> {
                                std::array<uint8_t, 32 + 32 + 32 + 1 + 32> pda_input{};
                                std::memcpy(pda_input.data(), owner.data(), 32);
                                std::memcpy(pda_input.data() + 32, token_prog.data(), 32);
                                std::memcpy(pda_input.data() + 64, usdc_mint.data(), 32);
                                pda_input[96] = 0xFF;
                                std::memcpy(pda_input.data() + 97, ata_prog.data(), 32);
                                return Fuchey::Crypto::sha256(
                                    std::span<const uint8_t>(pda_input.data(), pda_input.size()));
                            };

                            std::array<uint8_t, 32> sender_ata = derive_ata(
                                std::vector<uint8_t>(sender_pubkey.begin(), sender_pubkey.end()));
                            std::array<uint8_t, 32> recipient_ata = derive_ata(recipient_pubkey);

                            // Flush old events
                            Fuchey::Events::Event dummy_evt;
                            if (Fuchey::g_tx_confirm_queue) {
                                while (xQueueReceive(Fuchey::g_tx_confirm_queue, &dummy_evt, 0) == pdTRUE) {}
                            }

                            // Prompt for confirmation
                            uint64_t cents = static_cast<uint64_t>(amount * 100.0f);
                            Fuchey::Events::Event evt{};
                            evt.type = Fuchey::Events::EventType::TX_REQUEST;
                            evt.data.tx.amount_cents = cents;
                            evt.data.tx.tx_len = 0;
                            Fuchey::Events::post(Fuchey::Events::g_ui_queue, evt);

                            ESP_LOGI(CTAG, "[SEND USDC] Waiting for hardware button press or serial 'c' approval...");
                            ESP_LOGI(CTAG, "-------------------------------------------------");

                            Fuchey::Events::Event app_evt{};
                            bool got_response = false;
                            if (Fuchey::g_tx_confirm_queue) {
                                got_response = (xQueueReceive(Fuchey::g_tx_confirm_queue, &app_evt, pdMS_TO_TICKS(30000)) == pdTRUE);
                            }

                            if (!got_response || app_evt.type != Fuchey::Events::EventType::TX_APPROVED) {
                                ESP_LOGW(CTAG, "-------------------------------------------------");
                                ESP_LOGW(CTAG, "[SEND USDC] Transfer REJECTED or TIMED OUT by user.");
                                ESP_LOGW(CTAG, "-------------------------------------------------");
                                vTaskDelete(nullptr);
                                return;
                            }

                            ESP_LOGI(CTAG, "[SEND USDC] Transaction APPROVED! Fetching blockhash from Devnet...");

                            // Fetch blockhash
                            auto bh_resp = s_wifi_manager.post_json(
                                get_rpc_url(),
                                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getLatestBlockhash\",\"params\":[{\"commitment\":\"finalized\"}]}"
                            );

                            if (!bh_resp.success) {
                                ESP_LOGE(CTAG, "[SEND USDC] Failed to get latest blockhash from RPC (status %d)", bh_resp.status_code);
                                vTaskDelete(nullptr);
                                return;
                            }

                            std::string blockhash_str;
                            cJSON* bh_root = cJSON_Parse(bh_resp.body.c_str());
                            if (bh_root) {
                                cJSON* res = cJSON_GetObjectItem(bh_root, "result");
                                cJSON* val = res ? cJSON_GetObjectItem(res, "value") : nullptr;
                                cJSON* bh  = val ? cJSON_GetObjectItem(val, "blockhash") : nullptr;
                                if (bh && bh->valuestring) {
                                    blockhash_str = bh->valuestring;
                                }
                                cJSON_Delete(bh_root);
                            }

                            if (blockhash_str.empty()) {
                                ESP_LOGE(CTAG, "[SEND USDC] Error parsing blockhash: %.100s", bh_resp.body.c_str());
                                vTaskDelete(nullptr);
                                return;
                            }

                            auto blockhash_bytes = Fuchey::Crypto::Base58::decode(blockhash_str);
                            if (blockhash_bytes.size() != 32) {
                                ESP_LOGE(CTAG, "[SEND USDC] Invalid blockhash decode size (%d)", blockhash_bytes.size());
                                vTaskDelete(nullptr);
                                return;
                            }

                            // ── Build Solana message ──
                            // USDC has 6 decimals
                            uint64_t raw_amount = static_cast<uint64_t>(amount * 1000000.0f);
                            std::vector<uint8_t> msg;

                            // Header
                            msg.push_back(1); // num_required_signatures = 1
                            msg.push_back(0); // num_readonly_signed_accounts = 0
                            msg.push_back(1); // num_readonly_unsigned_accounts = 1 (Token Program)

                            // Accounts
                            msg.push_back(4); // 4 accounts
                            msg.insert(msg.end(), sender_pubkey.begin(), sender_pubkey.end()); // [0] signer
                            msg.insert(msg.end(), sender_ata.begin(), sender_ata.end());       // [1] source ATA
                            msg.insert(msg.end(), recipient_ata.begin(), recipient_ata.end()); // [2] dest ATA
                            msg.insert(msg.end(), token_prog.begin(), token_prog.end());       // [3] Token Program

                            // Blockhash
                            msg.insert(msg.end(), blockhash_bytes.begin(), blockhash_bytes.end());

                            // Instructions
                            msg.push_back(1); // 1 instruction
                            msg.push_back(3); // program_id_index = 3 (Token Program)
                            msg.push_back(3); // 3 account indices
                            msg.push_back(1); // accounts[0] = source ATA (index 1)
                            msg.push_back(2); // accounts[1] = dest ATA (index 2)
                            msg.push_back(0); // accounts[2] = owner (index 0)
                            msg.push_back(12); // data length = 12 bytes
                            // SPL Transfer: u32 LE tag (3) + u64 LE amount
                            msg.push_back(3); msg.push_back(0); msg.push_back(0); msg.push_back(0);
                            for (int i = 0; i < 8; i++) {
                                msg.push_back(static_cast<uint8_t>((raw_amount >> (i * 8)) & 0xFF));
                            }

                            // Sign
                            Fuchey::Crypto::Signature sig{};
                            auto sign_res = s_wallet_core.sign(msg, sig);
                            if (sign_res != Fuchey::WalletResult::OK) {
                                ESP_LOGE(CTAG, "[SEND USDC] Signing failed (err=%d)", static_cast<int>(sign_res));
                                vTaskDelete(nullptr);
                                return;
                            }

                            // Build wire transaction
                            std::vector<uint8_t> wire_tx;
                            wire_tx.push_back(1);
                            wire_tx.insert(wire_tx.end(), sig.begin(), sig.end());
                            wire_tx.insert(wire_tx.end(), msg.begin(), msg.end());

                            std::string base58_tx = Fuchey::Crypto::Base58::encode(wire_tx);

                            // Submit
                            char req_buf[2048];
                            snprintf(req_buf, sizeof(req_buf),
                                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"sendTransaction\",\"params\":[\"%s\",{\"encoding\":\"base58\"}]}",
                                     base58_tx.c_str());

                            ESP_LOGI(CTAG, "[SEND USDC] Broadcasting signed transaction to Solana Devnet...");
                            auto tx_resp = s_wifi_manager.post_json(get_rpc_url(), req_buf);

                            Fuchey::Events::Event result_evt{};
                            result_evt.data.tx.amount_cents = static_cast<uint64_t>(amount * 100.0f);
                            snprintf(reinterpret_cast<char*>(result_evt.data.tx.tx_data),
                                     sizeof(result_evt.data.tx.tx_data),
                                     "USDC:%.2f:%s", amount, recipient_str.c_str());

                            if (tx_resp.success) {
                                cJSON* tx_root = cJSON_Parse(tx_resp.body.c_str());
                                if (tx_root) {
                                    cJSON* tx_res = cJSON_GetObjectItem(tx_root, "result");
                                    cJSON* tx_err = cJSON_GetObjectItem(tx_root, "error");
                                    if (tx_res && tx_res->valuestring) {
                                        ESP_LOGI(CTAG, "=================================================");
                                        ESP_LOGI(CTAG, "  [SUCCESS] USDC Transfer Broadcast Complete!");
                                        ESP_LOGI(CTAG, "  Signature: %s", tx_res->valuestring);
                                        ESP_LOGI(CTAG, "  Explorer:  https://explorer.solana.com/tx/%s?cluster=devnet", tx_res->valuestring);
                                        ESP_LOGI(CTAG, "=================================================");
                                        result_evt.type = Fuchey::Events::EventType::TX_BROADCAST_OK;
                                    } else if (tx_err) {
                                        cJSON* msg_item = cJSON_GetObjectItem(tx_err, "message");
                                        const char* err_str = msg_item && msg_item->valuestring ? msg_item->valuestring : "Unknown RPC error";
                                        ESP_LOGE(CTAG, "  [TX ERROR] RPC Error: %s", err_str);
                                        snprintf(reinterpret_cast<char*>(result_evt.data.tx.tx_data),
                                                 sizeof(result_evt.data.tx.tx_data), "%s", err_str);
                                        result_evt.type = Fuchey::Events::EventType::TX_BROADCAST_FAIL;
                                    } else {
                                        ESP_LOGE(CTAG, "  [TX ERROR] Response: %.150s", tx_resp.body.c_str());
                                        snprintf(reinterpret_cast<char*>(result_evt.data.tx.tx_data),
                                                 sizeof(result_evt.data.tx.tx_data), "RPC parse error");
                                        result_evt.type = Fuchey::Events::EventType::TX_BROADCAST_FAIL;
                                    }
                                    cJSON_Delete(tx_root);
                                } else {
                                    ESP_LOGE(CTAG, "  [TX ERROR] Parse failure. Raw: %.150s", tx_resp.body.c_str());
                                    snprintf(reinterpret_cast<char*>(result_evt.data.tx.tx_data),
                                             sizeof(result_evt.data.tx.tx_data), "RPC parse failure");
                                    result_evt.type = Fuchey::Events::EventType::TX_BROADCAST_FAIL;
                                }
                            } else {
                                ESP_LOGE(CTAG, "  [TX ERROR] HTTP request failed (%d)", tx_resp.status_code);
                                snprintf(reinterpret_cast<char*>(result_evt.data.tx.tx_data),
                                         sizeof(result_evt.data.tx.tx_data), "HTTP %d", tx_resp.status_code);
                                result_evt.type = Fuchey::Events::EventType::TX_BROADCAST_FAIL;
                            }

                            Fuchey::Events::post(Fuchey::Events::g_ui_queue, result_evt);
                            vTaskDelete(nullptr);
                        }, "send_usdc_task", 20480, args, 4, nullptr);
                    } else {
                        delete args;
                        ESP_LOGW(CTAG, "Usage: send usdc <amount> <recipient_address>");
                    }

                // ── Interactive Send prompt ───────────────────
                } else if (strcmp(cmd, "send") == 0) {
                    ESP_LOGI(CTAG, "=================================================");
                    ESP_LOGI(CTAG, "  SEND TOKEN SELECTION:");
                    ESP_LOGI(CTAG, "    send sol  <amount> <recipient>");
                    ESP_LOGI(CTAG, "    send usdc <amount> <recipient>");
                    ESP_LOGI(CTAG, "  Example:");
                    ESP_LOGI(CTAG, "    send sol 0.25 7xKX...3b9Z");
                    ESP_LOGI(CTAG, "    send usdc 5.00 7xKX...3b9Z");
                    ESP_LOGI(CTAG, "=================================================");

                // ── Manual Weather update ──────────────────────
                } else if (strcmp(cmd, "weather") == 0) {
                    ESP_LOGI(CTAG, "[Weather] Fetching geolocation & weather...");
                    s_weather_service.update_now();

                // ── Help ──────────────────────────────────────
                } else if ((cmd[0] == 'h' || cmd[0] == '?') && len == 1) {
                    ESP_LOGI(CTAG, "  w <ssid> <pass>          WiFi connect & save");
                    ESP_LOGI(CTAG, "  wallet_create            Generate new wallet");
                    ESP_LOGI(CTAG, "  wallet_import <mnemonic> Import BIP39 mnemonic");
                    ESP_LOGI(CTAG, "  wallet_import <key>      Import hex/base58 private key");
                    ESP_LOGI(CTAG, "  wallet_selftest          Verify Ed25519 math");
                    ESP_LOGI(CTAG, "  wallet_info              Show current address");
                    ESP_LOGI(CTAG, "  balance                  Fetch live SOL & USDC balance");
                    ESP_LOGI(CTAG, "  ai <message>             Send prompt to AI Assistant");
                    ESP_LOGI(CTAG, "  ai_key <key>             Set OpenAI API key");
                    ESP_LOGI(CTAG, "  send                     Token transfer menu (SOL / USDC)");
                    ESP_LOGI(CTAG, "  send sol <amt> <to>      Transfer SOL");
                    ESP_LOGI(CTAG, "  send usdc <amt> <to>     Transfer USDC");
                    ESP_LOGI(CTAG, "  network                  Show / switch network (devnet/mainnet)");
                    ESP_LOGI(CTAG, "  airdrop [amount]         Request Devnet SOL airdrop");
                    ESP_LOGI(CTAG, "  weather                  Fetch weather & geolocation");
                    ESP_LOGI(CTAG, "  p                        Fetch SOL price");
                    ESP_LOGI(CTAG, "  c / 1                    CONFIRM button");
                    ESP_LOGI(CTAG, "  b / 2                    BACK button");

                } else {
                    ESP_LOGW(CTAG, "Unknown command: '%s'  (type 'h' for help)", cmd);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }, "console_task", 8192, nullptr, 4, nullptr, 0);

    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  Boot complete. Fuchey is running.");
    ESP_LOGI(TAG, "=================================================");
}
