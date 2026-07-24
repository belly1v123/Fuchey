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
#include "../lib/wallet/WalletCore.hpp"
#include "../lib/policy/SpendingPolicy.hpp"
#include "../lib/wallet_manager/WalletManager.hpp"
#include "../lib/wifi/WiFiManager.hpp"
#include "../lib/chat/AIManager.hpp"
#include "../lib/weather/WeatherService.hpp"
#include "../lib/price/PriceService.hpp"

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

// Global ref for button driver callback
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
                                        Fuchey::Buttons::PIN_BACK,
                                        Fuchey::Buttons::DEBOUNCE_MS,
                                        Fuchey::Buttons::LONG_PRESS_MS);

static Fuchey::WalletCore     s_wallet_core;
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

// ─── Helpers ──────────────────────────────────────────────

// Check if string is exactly 64 hex characters (raw private key)
static bool is_hex64(const char* s) {
    size_t len = strlen(s);
    if (len != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
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
        ESP_LOGI(TAG, "[OK] Button driver initialized (CONFIRM=GPIO%d)", Fuchey::Buttons::PIN_CONFIRM);
    }

    // 3. Initialize Core Security & Wallet Layer
    s_spending_policy.init();
    s_wallet_core.init();
    s_wallet_manager.init();
    ESP_LOGI(TAG, "[OK] Wallet core initialized — state: %s",
             s_wallet_core.has_wallet() ? "LOCKED (wallet found)" : "UNINITIALIZED (no wallet)");

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

    // Weather Task (Core 0)
    xTaskCreatePinnedToCore(Fuchey::WeatherService::task_entry, "weather_task",
                            Fuchey::Tasks::WEATHER_STACK, &s_weather_service,
                            Fuchey::Tasks::WEATHER_PRIORITY, nullptr, Fuchey::Tasks::UI_CORE);

    // Price Task (Core 0)
    xTaskCreatePinnedToCore(Fuchey::PriceService::task_entry, "price_task",
                            Fuchey::Tasks::PRICE_STACK, &s_price_service,
                            Fuchey::Tasks::PRICE_PRIORITY, nullptr, Fuchey::Tasks::UI_CORE);

    // Interactive Serial Console Task (Core 0)
    xTaskCreatePinnedToCore([](void*) {
        static constexpr const char* CTAG = "Console";

        ESP_LOGI(CTAG, "=================================================");
        ESP_LOGI(CTAG, "  Fuchey Interactive Serial Console Ready");
        ESP_LOGI(CTAG, "=================================================");
        ESP_LOGI(CTAG, "  Commands:");
        ESP_LOGI(CTAG, "    w <ssid> <pass>           WiFi connect & save");
        ESP_LOGI(CTAG, "    wallet_create              Generate new wallet");
        ESP_LOGI(CTAG, "    wallet_import <12 words>   Import BIP39 mnemonic");
        ESP_LOGI(CTAG, "    wallet_import <64hex>      Import raw private key");
        ESP_LOGI(CTAG, "    wallet_info                Show current address");
        ESP_LOGI(CTAG, "    p                          Force SOL price fetch");
        ESP_LOGI(CTAG, "    c / 1                      CONFIRM button");
        ESP_LOGI(CTAG, "    b / 2                      BACK button");
        ESP_LOGI(CTAG, "    h / ?                      Show this help");
        ESP_LOGI(CTAG, "=================================================");

        char line[300];
        while (true) {
            if (fgets(line, sizeof(line), stdin)) {
                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
                    line[--len] = '\0';
                }
                // Trim leading whitespace
                char* cmd = line;
                while (*cmd == ' ') ++cmd;
                len = strlen(cmd);
                if (len == 0) continue;

                // ── CONFIRM button ────────────────────────────
                if ((cmd[0] == 'c' || cmd[0] == '1') && len == 1) {
                    ESP_LOGI(CTAG, "[INPUT] CONFIRM");
                    Fuchey::ButtonState state{
                        .id = Fuchey::ButtonId::CONFIRM,
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

                            // JSON-RPC getBalance payload for SOL
                            char sol_req[256];
                            snprintf(sol_req, sizeof(sol_req),
                                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getBalance\",\"params\":[\"%s\"]}",
                                     address.c_str());

                            auto resp = s_wifi_manager.post_json(get_rpc_url(), sol_req);
                            if (resp.success) {
                                cJSON* root = cJSON_Parse(resp.body.c_str());
                                if (root) {
                                    cJSON* res = cJSON_GetObjectItem(root, "result");
                                    cJSON* val = res ? cJSON_GetObjectItem(res, "value") : nullptr;
                                    if (val && cJSON_IsNumber(val)) {
                                        double lamports = val->valuedouble;
                                        double sol_bal = lamports / 1000000000.0;
                                        ESP_LOGI(CTAG, "  SOL Balance:  %.6f SOL (%.0f lamports)", sol_bal, lamports);
                                    } else {
                                        ESP_LOGW(CTAG, "  SOL Balance:  0.000000 SOL (0 lamports)");
                                    }
                                    cJSON_Delete(root);
                                } else {
                                    ESP_LOGE(CTAG, "  SOL: JSON parse failed. Body: %.80s", resp.body.c_str());
                                }
                            } else {
                                ESP_LOGE(CTAG, "Failed to query SOL balance from RPC");
                            }

                            // JSON-RPC getTokenAccountsByOwner payload for USDC
                            char usdc_req[512];
                            snprintf(usdc_req, sizeof(usdc_req),
                                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTokenAccountsByOwner\","
                                     "\"params\":[\"%s\",{\"mint\":\"%s\"},{\"encoding\":\"jsonParsed\"}]}",
                                     address.c_str(), get_usdc_mint());

                            auto u_resp = s_wifi_manager.post_json(get_rpc_url(), usdc_req);
                            if (u_resp.success) {
                                cJSON* root = cJSON_Parse(u_resp.body.c_str());
                                double usdc_bal = 0.0;
                                if (root) {
                                    cJSON* res = cJSON_GetObjectItem(root, "result");
                                    cJSON* val = res ? cJSON_GetObjectItem(res, "value") : nullptr;
                                    if (cJSON_IsArray(val) && cJSON_GetArraySize(val) > 0) {
                                        cJSON* item0 = cJSON_GetArrayItem(val, 0);
                                        cJSON* account = cJSON_GetObjectItem(item0, "account");
                                        cJSON* data = account ? cJSON_GetObjectItem(account, "data") : nullptr;
                                        cJSON* parsed = data ? cJSON_GetObjectItem(data, "parsed") : nullptr;
                                        cJSON* info = parsed ? cJSON_GetObjectItem(parsed, "info") : nullptr;
                                        cJSON* t_amt = info ? cJSON_GetObjectItem(info, "tokenAmount") : nullptr;
                                        cJSON* ui_amt = t_amt ? cJSON_GetObjectItem(t_amt, "uiAmount") : nullptr;
                                        if (ui_amt && cJSON_IsNumber(ui_amt)) {
                                            usdc_bal = ui_amt->valuedouble;
                                        }
                                    }
                                    cJSON_Delete(root);
                                }
                                ESP_LOGI(CTAG, "  USDC Balance: $%.2f USDC", usdc_bal);
                            } else {
                                ESP_LOGE(CTAG, "Failed to query USDC balance from RPC");
                            }
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
                    } else {
                        ESP_LOGE(CTAG, "[Wallet] Creation FAILED (err=%d)", static_cast<int>(r));
                    }
                    ESP_LOGI(CTAG, "-------------------------------------------------");

                // ── wallet_import ─────────────────────────────
                } else if (strncmp(cmd, "wallet_import ", 14) == 0 && len > 14) {
                    const char* payload = cmd + 14;
                    // Skip leading spaces
                    while (*payload == ' ') ++payload;

                    ESP_LOGI(CTAG, "-------------------------------------------------");
                    Fuchey::WalletResult r;

                    // Auto-detect format: 64 hex chars = raw private key, else BIP39
                    if (is_hex64(payload)) {
                        ESP_LOGI(CTAG, "[Wallet] Detected: 64-char hex private key");
                        r = s_wallet_core.import_privkey_hex(payload);
                    } else {
                        int word_count = 0;
                        const char* p = payload;
                        while (*p) {
                            while (*p == ' ') ++p;
                            if (*p) { ++word_count; while (*p && *p != ' ') ++p; }
                        }
                        ESP_LOGI(CTAG, "[Wallet] Detected: BIP39 mnemonic (%d words)", word_count);
                        r = s_wallet_core.import(payload);
                    }

                    if (r == Fuchey::WalletResult::OK) {
                        auto addr = s_wallet_core.get_address();
                        ESP_LOGI(CTAG, "[Wallet] IMPORTED SUCCESSFULLY!");
                        ESP_LOGI(CTAG, "  Address: %s", addr ? addr->c_str() : "(error)");

                        std::string addr_str = addr ? *addr : "";
                        Fuchey::Events::Event evt{};
                        evt.type = Fuchey::Events::EventType::WALLET_IMPORTED;
                        Fuchey::Events::post(Fuchey::Events::g_wallet_queue, evt);
                        s_ui.mark_wallet_configured(addr_str.c_str());
                    } else {
                        const char* reason =
                            (r == Fuchey::WalletResult::ERR_INVALID_MNEMONIC) ? "invalid mnemonic" :
                            (r == Fuchey::WalletResult::ERR_INVALID_PRIVKEY)  ? "invalid private key (bad hex?)" :
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
                } else if (strncmp(cmd, "send sol ", 9) == 0 && len > 9) {
                    float amount = 0.0f;
                    char recipient[64] = {0};
                    if (sscanf(cmd + 9, "%f %63s", &amount, recipient) == 2) {
                        ESP_LOGI(CTAG, "-------------------------------------------------");
                        ESP_LOGI(CTAG, "[SEND SOL] Initiating transfer:");
                        ESP_LOGI(CTAG, "  Asset:     SOL");
                        ESP_LOGI(CTAG, "  Amount:    %.4f SOL", amount);
                        ESP_LOGI(CTAG, "  Recipient: %s", recipient);

                        // Convert SOL amount to estimated USD cents for spending policy check ($150/SOL default estimate if unfetched)
                        uint64_t cents = static_cast<uint64_t>(amount * 150.0f * 100.0f);
                        Fuchey::Events::Event evt{};
                        evt.type = Fuchey::Events::EventType::TX_REQUEST;
                        evt.data.tx.amount_cents = cents;
                        evt.data.tx.tx_len = 0;
                        Fuchey::Events::post(Fuchey::Events::g_ui_queue, evt);
                        ESP_LOGI(CTAG, "-------------------------------------------------");
                    } else {
                        ESP_LOGW(CTAG, "Usage: send sol <amount> <recipient_address>");
                    }

                // ── Send USDC ─────────────────────────────────
                } else if (strncmp(cmd, "send usdc ", 10) == 0 && len > 10) {
                    float amount = 0.0f;
                    char recipient[64] = {0};
                    if (sscanf(cmd + 10, "%f %63s", &amount, recipient) == 2) {
                        ESP_LOGI(CTAG, "-------------------------------------------------");
                        ESP_LOGI(CTAG, "[SEND USDC] Initiating SPL transfer:");
                        ESP_LOGI(CTAG, "  Asset:     USDC (SPL Token)");
                        ESP_LOGI(CTAG, "  Amount:    $%.2f USDC", amount);
                        ESP_LOGI(CTAG, "  Recipient: %s", recipient);

                        // 1 USDC = $1.00 = 100 cents
                        uint64_t cents = static_cast<uint64_t>(amount * 100.0f);
                        Fuchey::Events::Event evt{};
                        evt.type = Fuchey::Events::EventType::TX_REQUEST;
                        evt.data.tx.amount_cents = cents;
                        evt.data.tx.tx_len = 0;
                        Fuchey::Events::post(Fuchey::Events::g_ui_queue, evt);
                        ESP_LOGI(CTAG, "-------------------------------------------------");
                    } else {
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
                    ESP_LOGI(CTAG, "  wallet_import <12 words> Import BIP39 mnemonic");
                    ESP_LOGI(CTAG, "  wallet_import <64hex>    Import raw private key");
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
