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
    setenv("TZ", "UTC", 1);
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

                // ── Help ──────────────────────────────────────
                } else if ((cmd[0] == 'h' || cmd[0] == '?') && len == 1) {
                    ESP_LOGI(CTAG, "  w <ssid> <pass>          WiFi connect & save");
                    ESP_LOGI(CTAG, "  wallet_create            Generate new wallet");
                    ESP_LOGI(CTAG, "  wallet_import <12 words> Import BIP39 mnemonic");
                    ESP_LOGI(CTAG, "  wallet_import <64hex>    Import raw private key");
                    ESP_LOGI(CTAG, "  wallet_info              Show current address");
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
