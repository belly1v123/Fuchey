// ============================================================
// Fuchey — main.cpp
// System entry point. Boot sequence, queue allocation,
// subsystem initialization, and FreeRTOS task pinning.
// ============================================================

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

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

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  Fuchey Firmware v%s Booting...", Fuchey::FW_VERSION);
    ESP_LOGI(TAG, "  Target MCU: ESP32-S3 (16MB Flash, 8MB OPI PSRAM)");
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

    if (!s_display.init()) {
        ESP_LOGE(TAG, "Display initialization failed!");
    }
    s_ui.init();

    if (!s_buttons.init(Fuchey::Events::g_button_queue)) {
        ESP_LOGE(TAG, "Button Driver initialization failed!");
    }

    // 3. Initialize Core Security & Wallet Layer
    s_spending_policy.init();
    s_wallet_core.init();
    s_wallet_manager.init();

    // 4. Initialize Network & Services
    s_wifi_manager.init();
    s_ai_manager.init();
    s_weather_service.init();
    s_price_service.init();

    // Attempt WiFi auto-connect from NVS
    s_wifi_manager.connect_from_nvs();

    // 5. Spawn FreeRTOS Tasks with Strict CPU Core Pinning & Priorities
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
        ESP_LOGI("Console", "=================================================");
        ESP_LOGI("Console", "  Fuchey Interactive Serial Console Ready!");
        ESP_LOGI("Console", "  Commands:");
        ESP_LOGI("Console", "    'c' or '1'        : Simulate CONFIRM / Next Screen");
        ESP_LOGI("Console", "    'b' or '2'        : Simulate BACK / Idle Screen");
        ESP_LOGI("Console", "    'w <ssid> <pass>' : Connect WiFi");
        ESP_LOGI("Console", "    'p'               : Fetch live SOL price");
        ESP_LOGI("Console", "=================================================");

        char line[128];
        while (true) {
            if (fgets(line, sizeof(line), stdin)) {
                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
                    line[--len] = '\0';
                }
                if (len == 0) continue;

                if (line[0] == 'c' || line[0] == '1') {
                    ESP_LOGI("Console", "-> [INPUT] CONFIRM command");
                    Fuchey::ButtonState state{.id = Fuchey::ButtonId::CONFIRM, .event = Fuchey::ButtonEvent::PRESS, .timestamp_ms = 0};
                    xQueueSend(::g_button_queue_ref, &state, 0);
                } else if (line[0] == 'b' || line[0] == '2') {
                    ESP_LOGI("Console", "-> [INPUT] BACK command");
                    Fuchey::ButtonState state{.id = Fuchey::ButtonId::BACK, .event = Fuchey::ButtonEvent::PRESS, .timestamp_ms = 0};
                    xQueueSend(::g_button_queue_ref, &state, 0);
                } else if (line[0] == 'w' && len > 2) {
                    char ssid[64] = {0}, pass[64] = {0};
                    if (sscanf(line + 2, "%63s %63s", ssid, pass) >= 1) {
                        ESP_LOGI("Console", "-> [INPUT] Connecting WiFi to SSID='%s'", ssid);
                        s_wifi_manager.connect(ssid, pass);
                    }
                } else if (line[0] == 'p') {
                    ESP_LOGI("Console", "-> [INPUT] Fetching live SOL price...");
                    s_price_service.update_now();
                } else if (line[0] == 'h' || line[0] == '?') {
                    ESP_LOGI("Console", "Commands: 'c'=Confirm, 'b'=Back, 'w <ssid> <pass>'=WiFi, 'p'=Fetch Price");
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }, "console_task", 4096, nullptr, 4, nullptr, 0);

    ESP_LOGI(TAG, "Boot sequence completed cleanly. Fuchey is active.");
}
